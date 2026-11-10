#include "Parser.h"
#include <memory>
#include <functional>
#include <limits>
#include <unordered_set>
#include "ast/Decl.h"
#include "ast/Mangle.h"
#include "frontend/OperatorPrecedence.h"
#include "frontend/Token.h"

namespace {

// `atomic` is a keyword, but it must remain usable as a module path component
// (`import std.atomic;`). Module names accept any identifier-shaped lexeme.
static bool isModuleNameToken(const Token& token) {
    if (token.type == TokenType::TOKEN_IDENTIFIER) return true;
    const std::string& s = token.lexeme;
    if (s.empty()) return false;
    auto isStart = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    auto isContinue = [&](unsigned char c) {
        return isStart(c) || (c >= '0' && c <= '9');
    };
    if (!isStart(static_cast<unsigned char>(s[0]))) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        if (!isContinue(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

// CTX-01: compile_time query argument policy.
//   None       — every argument is an ordinary expression
//   FirstType  — first argument may be a type-id; the rest are expressions
//   AllTypes   — every argument may be a type-id (is_same/is_convertible/...)
enum class CTArgPolicy { None, FirstType, AllTypes };

CTArgPolicy ctArgPolicyFor(const std::string& member) {
    static const std::unordered_set<std::string> kAllTypes = {
        "is_same", "is_convertible", "is_assignable", "is_base_of",
    };
    // (is_same/is_convertible/is_assignable/is_base_of all take two types.)
    static const std::unordered_set<std::string> kFirstType = {
        // layout / reflection
        "size_of", "align_of", "offset_of", "name_of", "fields_of",
        "attributes_of", "bit_size_of", "padding_of", "is_packed",
        "type_of", "array_of", "pointer_to", "const_of",
        "type_name_of", "type_kind_of", "layout_size_of", "layout_align_of",
        // type predicates
        "is_integer", "is_float", "is_bool", "is_pointer", "is_array",
        "is_aggregate", "is_class", "is_struct", "is_union", "is_enum",
        "is_void", "is_signed",
        // numeric limits
        "int_min", "int_max", "uint_max", "bit_width",
        "float_epsilon", "float_max", "float_digits",
        // field / attribute reflection
        "has_field", "field_count_of", "field_name_at", "field_type_name_of",
        "field_size_of", "field_offset_of", "field_is_bitfield",
        "field_bit_width_of", "field_annotation_count_of",
        "field_annotation_name_at", "field_annotation_arg_of",
        "has_attribute", "attribute_count_of", "attribute_name_at",
        "attribute_arg_of",
        // enum reflection
        "enum_count_of", "enum_name_at", "enum_value_at", "enum_value_of",
        "enum_name_of", "enum_underlying_name",
        // class / template reflection
        "method_count_of", "has_method", "method_name_at",
        "method_param_count", "method_param_type_name",
        "method_return_type_name", "base_name_of",
        "template_name_of", "template_arg_count", "template_arg_name",
        // assertions with a leading type
        "assert_size", "assert_align", "assert_offset",
    };
    if (kAllTypes.count(member)) return CTArgPolicy::AllTypes;
    if (kFirstType.count(member)) return CTArgPolicy::FirstType;
    return CTArgPolicy::None;
}

bool isCompileTimeChainRoot(const ExprAST* expr) {
    const ExprAST* cur = expr;
    while (auto* ma = dynamic_cast<const MemberAccessExprAST*>(cur)) {
        cur = ma->object.get();
    }
    auto* var = dynamic_cast<const VariableExprAST*>(cur);
    return var && var->name == "compile_time";
}


// Mangle a (possibly qualified) type name to a symbol-table key:
// `A::B::T` / `A.B.T` -> `A_B_T`.
std::string mangleQualifiedTypeName(const std::string& name) {    std::string out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == ':' && i + 1 < name.size() && name[i + 1] == ':') {
            out.push_back('_');
            ++i;
        } else if (name[i] == '.') {
            out.push_back('_');
        } else {
            out.push_back(name[i]);
        }
    }
    return out;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void appendUtf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Decode C escape sequences from a string literal body (quotes already removed).
std::string decodeEscapes(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != '\\' || i + 1 >= s.size()) {
            out.push_back(c);
            continue;
        }
        char e = s[++i];
        switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '0': out.push_back('\0'); break;
            case 'a': out.push_back('\a'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'v': out.push_back('\v'); break;
            case '\\': out.push_back('\\'); break;
            case '\'': out.push_back('\''); break;
            case '"': out.push_back('"'); break;
            case '?': out.push_back('?'); break;
            case 'x': {
                int v = 0, n = 0;
                while (i + 1 < s.size() && n < 2 && hexDigit(s[i + 1]) >= 0) {
                    v = v * 16 + hexDigit(s[++i]);
                    ++n;
                }
                out.push_back(static_cast<char>(v));
                break;
            }
            case 'u': {
                unsigned int cp = 0;
                if (i + 1 < s.size() && s[i + 1] == '{') {
                    ++i; // consume '{'
                    while (i + 1 < s.size() && s[i + 1] != '}') {
                        int d = hexDigit(s[++i]);
                        if (d >= 0) cp = cp * 16 + static_cast<unsigned int>(d);
                    }
                    if (i + 1 < s.size() && s[i + 1] == '}') ++i; // consume '}'
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                out.push_back(e);
                break;
        }
    }
    return out;
}

// LEX-16: strip the string literal's enclosing quotes before escape decoding.
// A multiline token (`"""..."""`) carries three quotes per side; a regular or
// raw string carries one. Raw strings (`r"..."`) are handled by their own
// branch and never reach here with the multiline flag set.
std::string stripStringQuotes(const std::string& s, bool multiline) {
    size_t n = multiline ? 3 : 1;
    if (s.size() < 2 * n) return s;
    return s.substr(n, s.size() - 2 * n);
}

// SG-07: decode one string token. Adjacent literals are concatenated by the
// parser (after this helper has produced their decoded bytes).
std::string decodeStringToken(const Token& token, StringLiteralPrefix& prefix) {
    const std::string& lex = token.lexeme;
    std::string val;
    if (lex.size() >= 2 && lex[0] == 'R' && lex[1] == '"') {
        // SG-18: C++ raw string R"delim(body)delim" — no escape processing.
        const size_t open = lex.find('(', 2);
        if (open != std::string::npos) {
            const std::string delim = lex.substr(2, open - 2);
            const size_t suffixLen = delim.size() + 2; // ')' + delim + '"'
            if (lex.size() >= open + 1 + suffixLen) {
                val = lex.substr(open + 1, lex.size() - suffixLen - (open + 1));
            }
        }
        prefix = StringLiteralPrefix::None;
    } else if (lex.size() >= 2 && lex[0] == 'r' && lex[1] == '"') {
        if (lex.size() >= 3) val = lex.substr(2, lex.size() - 3);
        prefix = StringLiteralPrefix::None;
    } else if (prefix == StringLiteralPrefix::None &&
               lex.size() >= 2 && lex.front() == '"' && lex.back() == '"') {
        val = decodeEscapes(stripStringQuotes(lex, token.isMultilineString));
    } else if (prefix != StringLiteralPrefix::None) {
        // u8/u16/u32 prefix: lexeme is `u8"hi"` — strip prefix + quotes.
        size_t q = lex.find('"');
        if (q != std::string::npos && lex.size() >= q + 2 && lex.back() == '"') {
            std::string body = lex.substr(q + 1, lex.size() - q - 2);
            val = decodeEscapes(body);
        }
    }
    return val;
}

// Evaluate an integer constant expression used for enumerator values and
// similar compile-time integers. Supports literals, unary +/-/~, the usual
// binary operators (TYP-25), and named constants supplied by the parser
// (enum values / const integer variables). Returns false otherwise.
using IntConstLookup = std::function<bool(const std::string&, long long&)>;

bool evalIntConstExpr(const ExprAST* expr, long long& out,
                      const IntConstLookup& lookup = {}) {
    if (!expr) return false;
    if (auto* n = dynamic_cast<const NumberExprAST*>(expr)) {
        out = n->value;
        return true;
    }
    if (auto* c = dynamic_cast<const CharExprAST*>(expr)) {
        out = static_cast<unsigned char>(c->value);
        return true;
    }
    if (auto* b = dynamic_cast<const BoolLiteralExprAST*>(expr)) {
        out = b->value ? 1 : 0;
        return true;
    }
    if (auto* v = dynamic_cast<const VariableExprAST*>(expr)) {
        return lookup && lookup(v->name, out);
    }
    if (auto* u = dynamic_cast<const UnaryExprAST*>(expr)) {
        long long v = 0;
        if (!evalIntConstExpr(u->operand.get(), v, lookup)) return false;
        switch (u->op) {
            case UnaryOp::Plus:   out = v;  return true;
            case UnaryOp::Minus:
                out = static_cast<long long>(
                    uint64_t{0} - static_cast<uint64_t>(v));
                return true;
            case UnaryOp::BitNot: out = ~v; return true;
            default: return false;
        }
    }
    if (auto* b = dynamic_cast<const BinaryExprAST*>(expr)) {
        long long l = 0, r = 0;
        if (!evalIntConstExpr(b->left.get(), l, lookup) ||
            !evalIntConstExpr(b->right.get(), r, lookup)) {
            return false;
        }
        // Bit-pattern arithmetic: signed overflow is UB in C++ and would
        // crash/trap for e.g. INT64_MIN / -1 during parsing.
        const uint64_t ul = static_cast<uint64_t>(l);
        const uint64_t ur = static_cast<uint64_t>(r);
        switch (b->op) {
            case BinaryOp::Add:
                out = static_cast<long long>(ul + ur); return true;
            case BinaryOp::Sub:
                out = static_cast<long long>(ul - ur); return true;
            case BinaryOp::Mul:
                out = static_cast<long long>(ul * ur); return true;
            case BinaryOp::Div:
                if (r == 0) return false;
                if (l == std::numeric_limits<long long>::min() && r == -1) {
                    out = l;  // two's-complement wraparound
                } else {
                    out = l / r;
                }
                return true;
            case BinaryOp::Mod:
                if (r == 0) return false;
                if (l == std::numeric_limits<long long>::min() && r == -1) {
                    out = 0;
                } else {
                    out = l % r;
                }
                return true;
            case BinaryOp::BitAnd: out = l & r; return true;
            case BinaryOp::BitOr:  out = l | r; return true;
            case BinaryOp::BitXor: out = l ^ r; return true;
            case BinaryOp::LShift:
                out = static_cast<long long>(ul << (static_cast<unsigned>(r) & 63));
                return true;
            case BinaryOp::RShift:
                out = static_cast<long long>(ul >> (static_cast<unsigned>(r) & 63));
                return true;
            default: return false;
        }
    }
    return false;
}

bool isIntegerScalarType(const Type* type) {
    if (!type) return false;
    switch (type->kind) {
        case TypeKind::Char:
        case TypeKind::Bool:
        case TypeKind::Int8:
        case TypeKind::Int16:
        case TypeKind::Int32:
        case TypeKind::Int64:
        case TypeKind::Int128:
        case TypeKind::UInt8:
        case TypeKind::UInt16:
        case TypeKind::UInt32:
        case TypeKind::UInt64:
        case TypeKind::UInt128:
        case TypeKind::ISize:
        case TypeKind::USize:
            return true;
        default:
            return false;
    }
}

} // namespace

static Type* stripDeclaratorType(Type* type);

const std::vector<Diagnostic>& Parser::getErrors() const {
    return m_errors;
}

void Parser::error(const std::string& msg, const Token& token) {
    m_errors.emplace_back(Diagnostic::Level::Error, msg, token.filename, token.line, token.column);
}

void Parser::error(DiagnosticCode code, const std::string& msg, const Token& token) {
    m_errors.emplace_back(Diagnostic::Level::Error, code, msg, token.filename, token.line, token.column);
}

void Parser::errorUnexpected(const std::string& expected) {
    auto token = peek();
    if (token) {
        std::string msg = "unexpected token '" + token->lexeme + "', " + expected;
        error(msg, *token);
    } else {
        errorUnexpectedEOF(expected);
    }
}

void Parser::errorUnexpectedEOF(const std::string& expected) {
    Token dummy;
    dummy.filename = m_tokens.empty() ? "" : m_tokens.back().filename;
    dummy.line = m_tokens.empty() ? 0 : m_tokens.back().line;
    dummy.column = m_tokens.empty() ? 0 : m_tokens.back().column;
    error("unexpected end of file, " + expected, dummy);
}

bool Parser::expect(TokenType type, const std::string& msg) {
    if (match(type)) {
        return true;
    }
    auto token = peek();
    if (token) {
        error(msg, *token);
    } else {
        errorUnexpectedEOF(msg);
    }
    return false;
}

std::string Parser::tokenTypeName(TokenType type) const {
    switch (type) {
        case TokenType::TOKEN_IDENTIFIER:    return "identifier";
        case TokenType::TOKEN_NUMBER:        return "number";
        case TokenType::TOKEN_STRING:        return "string literal";
        case TokenType::TOKEN_CHAR:          return "character literal";
        case TokenType::TOKEN_FLOAT:         return "'float'";
        case TokenType::TOKEN_CHAR_KW:       return "'char'";
        case TokenType::TOKEN_VOID:          return "'void'";
        case TokenType::TOKEN_IF:            return "'if'";
        case TokenType::TOKEN_ELSE:          return "'else'";
        case TokenType::TOKEN_FOR:           return "'for'";
        case TokenType::TOKEN_WHILE:         return "'while'";
        case TokenType::TOKEN_DO:            return "'do'";
        case TokenType::TOKEN_RETURN:        return "'return'";
        case TokenType::TOKEN_BREAK:         return "'break'";
        case TokenType::TOKEN_CONTINUE:      return "'continue'";
        case TokenType::TOKEN_STRUCT:        return "'struct'";
        case TokenType::TOKEN_UNION:         return "'union'";
        case TokenType::TOKEN_ENUM:          return "'enum'";
        case TokenType::TOKEN_TYPEDEF:       return "'typedef'";
        case TokenType::TOKEN_TEMPLATE:      return "'template'";
        case TokenType::TOKEN_TYPENAME:      return "'typename'";
        case TokenType::TOKEN_SIZEOF:        return "'sizeof'";
        case TokenType::TOKEN_OPERATOR:      return "'operator'";
        case TokenType::TOKEN_SEMICOLON:     return "';'";
        case TokenType::TOKEN_COMMA:         return "','";
        case TokenType::TOKEN_LBRACE:        return "'{'";
        case TokenType::TOKEN_RBRACE:        return "'}'";
        case TokenType::TOKEN_LPAREN:        return "'('";
        case TokenType::TOKEN_RPAREN:        return "')'";
        case TokenType::TOKEN_LBRACKET:      return "'['";
        case TokenType::TOKEN_RBRACKET:      return "']'";
        case TokenType::TOKEN_ASSIGN:        return "'='";
        case TokenType::TOKEN_COLON:         return "':'";
        case TokenType::TOKEN_EOS:           return "end of file";
        default:                             return "token";
    }
}

std::optional<Token> Parser::peek() const {
    if (eof()) {
        return {};
    }

    return std::make_optional(m_tokens[m_currentTokenPos]);
}

std::optional<Token> Parser::advance() {
    if (eof()) {
        return {};
    }

    return std::make_optional(m_tokens[m_currentTokenPos++]);
}

bool Parser::eof() const {
    return m_currentTokenPos >= m_tokens.size();
}

std::optional<Token> Parser::match(TokenType type) {
    if (eof()) {
        return {};
    }

    auto token = peek();
    if (token->type == type) {
        advance();
        return token;
    }

    return {};
}

// ========== Precedence & Associativity ==========

// Precedence/associativity live in the normative table (cmm::getOperatorInfo),
// which mirrors docs/spec/grammar.ebnf §9 and is pinned by unit tests.
int Parser::getPrecedence(TokenType op) const {
    return cmm::getOperatorInfo(op).precedence;
}

bool Parser::isRightAssociative(TokenType op) const {
    return cmm::getOperatorInfo(op).rightAssociative;
}

// ========== Token to Operator Conversion ==========

BinaryOp Parser::tokenTypeToBinaryOp(TokenType type) const {
    switch (type) {
        case TokenType::TOKEN_PLUS:     return BinaryOp::Add;
        case TokenType::TOKEN_MINUS:    return BinaryOp::Sub;
        case TokenType::TOKEN_STAR:     return BinaryOp::Mul;
        case TokenType::TOKEN_SLASH:    return BinaryOp::Div;
        case TokenType::TOKEN_PERCENT:  return BinaryOp::Mod;
        case TokenType::TOKEN_EQ:       return BinaryOp::Eq;
        case TokenType::TOKEN_NOT_EQ:   return BinaryOp::NotEq;
        case TokenType::TOKEN_LT:       return BinaryOp::Lt;
        case TokenType::TOKEN_GT:       return BinaryOp::Gt;
        case TokenType::TOKEN_LE:       return BinaryOp::Le;
        case TokenType::TOKEN_GE:       return BinaryOp::Ge;
        case TokenType::TOKEN_AND:      return BinaryOp::And;
        case TokenType::TOKEN_OR:       return BinaryOp::Or;
        case TokenType::TOKEN_BIT_AND:  return BinaryOp::BitAnd;
        case TokenType::TOKEN_BIT_OR:   return BinaryOp::BitOr;
        case TokenType::TOKEN_CARET:    return BinaryOp::BitXor;
        case TokenType::TOKEN_LSHIFT:   return BinaryOp::LShift;
        case TokenType::TOKEN_RSHIFT:   return BinaryOp::RShift;
        default:                        return BinaryOp::Invalid;
    }
}

AssignOp Parser::tokenTypeToAssignOp(TokenType type) const {
    switch (type) {
        case TokenType::TOKEN_ASSIGN:       return AssignOp::Assign;
        case TokenType::TOKEN_PLUS_EQ:      return AssignOp::AddAssign;
        case TokenType::TOKEN_MINUS_EQ:     return AssignOp::SubAssign;
        case TokenType::TOKEN_STAR_EQ:      return AssignOp::MulAssign;
        case TokenType::TOKEN_SLASH_EQ:     return AssignOp::DivAssign;
        case TokenType::TOKEN_PERCENT_EQ:   return AssignOp::ModAssign;
        case TokenType::TOKEN_AMP_EQ:       return AssignOp::BitAndAssign;
        case TokenType::TOKEN_PIPE_EQ:      return AssignOp::BitOrAssign;
        case TokenType::TOKEN_CARET_EQ:     return AssignOp::BitXorAssign;
        case TokenType::TOKEN_LSHIFT_EQ:    return AssignOp::LShiftAssign;
        case TokenType::TOKEN_RSHIFT_EQ:    return AssignOp::RShiftAssign;
        default:                            return AssignOp::Assign;
    }
}

// ========== Unary Operators ==========

// AGG-03 + PAR-12: true if `t` is an anonymous (unnamed) struct or union
// type — either the legacy empty-name shape or the synthetic `__anon_<N>`
// name (PAR-12 assigns unique synthetic names so each anonymous definition
// is a distinct type). Both must be treated as anonymous for member
// promotion.
static bool isAnonymousAggregate(Type* t) {
    auto isAnon = [](const std::string& n) {
        return n.empty() || n.rfind("__anon_", 0) == 0;
    };
    if (!t) return false;
    if (t->kind == TypeKind::Struct) return isAnon(static_cast<StructType*>(t)->name);
    if (t->kind == TypeKind::Union) return isAnon(static_cast<UnionType*>(t)->name);
    return false;
}

// AGG-03: promote the fields/members of an anonymous struct/union into `out`
// (C11 member promotion). Returns false on a member-name collision.
static bool promoteAnonymousMembers(std::vector<FieldInfo>& out, Type* agg) {
    std::vector<FieldInfo>* src = nullptr;
    if (agg->kind == TypeKind::Struct) src = &static_cast<StructType*>(agg)->fields;
    else if (agg->kind == TypeKind::Union) src = &static_cast<UnionType*>(agg)->members;
    if (!src) return false;
    for (const auto& m : *src) {
        for (const auto& existing : out) {
            if (existing.name == m.name) return false;
        }
        out.push_back(m);
    }
    return true;
}

static bool isUnaryOp(TokenType type) {
    switch (type) {
        case TokenType::TOKEN_MINUS:
        case TokenType::TOKEN_PLUS:
        case TokenType::TOKEN_NOT:
        case TokenType::TOKEN_TILDE:
        case TokenType::TOKEN_STAR:
        case TokenType::TOKEN_BIT_AND:
        case TokenType::TOKEN_PLUS_PLUS:
        case TokenType::TOKEN_MINUS_MINUS:
        case TokenType::TOKEN_SIZEOF:
            return true;
        default:
            return false;
    }
}

static UnaryOp tokenToUnaryOp(TokenType type) {
    switch (type) {
        case TokenType::TOKEN_MINUS:    return UnaryOp::Minus;
        case TokenType::TOKEN_PLUS:     return UnaryOp::Plus;
        case TokenType::TOKEN_NOT:      return UnaryOp::Not;
        case TokenType::TOKEN_TILDE:    return UnaryOp::BitNot;
        case TokenType::TOKEN_STAR:     return UnaryOp::Deref;
        case TokenType::TOKEN_BIT_AND:  return UnaryOp::AddressOf;
        case TokenType::TOKEN_PLUS_PLUS: return UnaryOp::PreInc;
        case TokenType::TOKEN_MINUS_MINUS: return UnaryOp::PreDec;
        default:                        return UnaryOp::Plus;
    }
}

// ========== Postfix Operators ==========

static bool isPostfixOp(TokenType type) {
    switch (type) {
        case TokenType::TOKEN_PLUS_PLUS:
        case TokenType::TOKEN_MINUS_MINUS:
        case TokenType::TOKEN_LPAREN:
        case TokenType::TOKEN_LBRACKET:
        case TokenType::TOKEN_DOT:
        case TokenType::TOKEN_ARROW:
            return true;
        default:
            return false;
    }
}

// ========== Parsing ==========

void Parser::applyLocation(ASTNode* node, const Token* tok) {
    if (node && tok) {
        node->setLocation(tok->filename, tok->line, tok->column);
    }
}

std::unique_ptr<ExprAST> Parser::parseUnary() {
    auto startTok = peek();
    auto expr = parseUnaryImpl();
    if (expr) {
        applyLocation(expr.get(), startTok ? &*startTok : nullptr);
    }
    return expr;
}

std::unique_ptr<ExprAST> Parser::parseUnaryImpl() {
    auto token = peek();
    if (!token) {
        return nullptr;
    }

    // Explicit cast operators: `static_cast<T>(x)` / `reinterpret_cast<T>(x)`
    // (LEX-11 / PAR-18 / DEC-18). They are contextual keywords: recognized only
    // when immediately followed by `<`, so the names remain usable elsewhere.
    if (token->type == TokenType::TOKEN_IDENTIFIER &&
        (token->lexeme == "static_cast" || token->lexeme == "reinterpret_cast") &&
        m_currentTokenPos + 1 < m_tokens.size() &&
        m_tokens[m_currentTokenPos + 1].type == TokenType::TOKEN_LT) {
        const bool isReinterpret = token->lexeme == "reinterpret_cast";
        advance(); // cast keyword
        advance(); // '<'
        Type* castType = parseTypeId();
        if (!castType) return nullptr;
        if (!expect(TokenType::TOKEN_GT, "expected '>' after cast type")) return nullptr;
        if (!expect(TokenType::TOKEN_LPAREN, "expected '(' after cast type")) return nullptr;
        auto operand = parseExpr(2);
        if (!operand) return nullptr;
        if (!expect(TokenType::TOKEN_RPAREN, "expected ')' after cast operand")) return nullptr;
        return std::make_unique<CastExprAST>(
            castType, std::move(operand),
            isReinterpret ? CastKind::Reinterpret : CastKind::Static);
    }

    // Inline asm (P1-08 / PAR-16): `asm [volatile] ("tpl" : outs : ins : clobbers)`.
    // Consumes `asm`; parseAsmExpr handles the rest.
    if (token->type == TokenType::TOKEN_ASM) {
        advance();
        return parseAsmExpr();
    }

    // Handle parenthesized expressions and cast expressions
    if (token->type == TokenType::TOKEN_LPAREN) {
        advance(); // consume '('

        // Try to detect a cast: (type) expr. Speculatively parse a full type and
        // require the closing ')'; otherwise reparse as a parenthesized
        // expression. Using parseType (rather than a fixed keyword list) allows
        // pointers, qualifiers, and named types: (int*)p, (struct S*)p, (void*)
        // handles, etc. (MEM-10 / P0-02).
        if (isTypeStart() || check(TokenType::TOKEN_CONST) ||
            check(TokenType::TOKEN_VOLATILE) || check(TokenType::TOKEN_ATOMIC)) {
            size_t saved = m_currentTokenPos;
            // SG-04: casts take a full type-id, including abstract function
            // pointers and array types (not just parseType's simple suffix).
            Type* castType = parseTypeId();
            if (castType && match(TokenType::TOKEN_RPAREN)) {
                // PAR-08: C99 compound literal — `(type){ init-list }`.
                if (check(TokenType::TOKEN_LBRACE)) {
                    advance(); // consume '{'
                    std::vector<std::unique_ptr<ExprAST>> initList;
                    std::vector<std::vector<Designator>> designatorList;
                    if (!check(TokenType::TOKEN_RBRACE)) {
                        while (true) {
                            // C99 designated initializers inside compound
                            // literals: same syntax as variable init lists.
                            std::vector<Designator> designators;
                            while (check(TokenType::TOKEN_DOT) ||
                                   check(TokenType::TOKEN_LBRACKET)) {
                                Designator d;
                                if (match(TokenType::TOKEN_DOT)) {
                                    d.kind = Designator::Kind::Field;
                                    auto id = match(TokenType::TOKEN_IDENTIFIER);
                                    if (!id) {
                                        errorUnexpected("expected field name after '.' in designated initializer");
                                        m_currentTokenPos = saved;
                                        return nullptr;
                                    }
                                    d.field = id->lexeme;
                                } else {
                                    advance(); // '['
                                    d.kind = Designator::Kind::Index;
                                    d.indexExpr = parseExpr();
                                    if (!d.indexExpr) {
                                        m_currentTokenPos = saved;
                                        return nullptr;
                                    }
                                    if (!expect(TokenType::TOKEN_RBRACKET,
                                                "expected ']' after designator index")) {
                                        m_currentTokenPos = saved;
                                        return nullptr;
                                    }
                                }
                                designators.push_back(std::move(d));
                            }
                            if (!designators.empty()) {
                                if (!match(TokenType::TOKEN_ASSIGN)) {
                                    errorUnexpected("expected '=' after designator");
                                    m_currentTokenPos = saved;
                                    return nullptr;
                                }
                            }
                            auto elem = parseExpr(2); // minPrec=2: ',' separates
                            if (!elem) {
                                m_currentTokenPos = saved;
                                return nullptr;
                            }
                            initList.push_back(std::move(elem));
                            designatorList.push_back(std::move(designators));
                            if (!match(TokenType::TOKEN_COMMA)) break;
                        }
                    }
                    if (!expect(TokenType::TOKEN_RBRACE,
                                "expected '}' to end compound literal")) {
                        m_currentTokenPos = saved;
                        return nullptr;
                    }
                    auto il = std::make_unique<InitializerListExprAST>(
                        std::move(initList));
                    il->designators = std::move(designatorList);
                    return std::make_unique<CompoundLiteralExprAST>(
                        castType, std::move(il));
                }
                auto operand = parseUnary();
                if (!operand) {
                    return nullptr;
                }
                // Match the unary precedence rule: postfix binds tighter than
                // the cast, so `(int64)compile_time.int_max(int32)` parses as
                // `(int64)(compile_time.int_max(int32))`.
                operand = parsePostfix(std::move(operand));
                return std::make_unique<CastExprAST>(castType, std::move(operand));
            }
            m_currentTokenPos = saved; // not a cast; reparse as an expression
        }

        // Not a cast, parse as parenthesized expression
        auto expr = parseExpr();
        if (!expr) {
            return nullptr;
        }
        expect(TokenType::TOKEN_RPAREN, "expected ')' after expression");
        return expr;
    }

    // Handle sizeof
    if (token->type == TokenType::TOKEN_SIZEOF) {
        advance(); // consume sizeof

        if (match(TokenType::TOKEN_LPAREN)) {
            // Try `sizeof(type)` first, then fall back to `sizeof(expr)`.
            if (isTypeStart() || check(TokenType::TOKEN_CONST) ||
                check(TokenType::TOKEN_VOLATILE) || check(TokenType::TOKEN_ATOMIC)) {
                size_t savedPos = m_currentTokenPos;
                Type* sizeofType = parseTypeId();
                if (sizeofType && check(TokenType::TOKEN_RPAREN)) {
                    advance(); // consume ')'
                    return std::make_unique<SizeofExprAST>(sizeofType);
                }
                // Not actually a type (e.g. a variable sharing a type name);
                // rewind and parse it as an expression instead.
                m_currentTokenPos = savedPos;
            }

            auto operand = parseExpr();
            if (!operand) {
                return nullptr;
            }
            expect(TokenType::TOKEN_RPAREN, "expected ')' after sizeof operand");
            return std::make_unique<SizeofExprAST>(nullptr, std::move(operand));
        }

        // sizeof expr
        auto operand = parseUnary();
        if (!operand) {
            return nullptr;
        }
        return std::make_unique<SizeofExprAST>(nullptr, std::move(operand));
    }

    // PAR-14: `alignof(T)` — type argument only, alignment query.
    if (token->type == TokenType::TOKEN_ALIGNOF) {
        advance(); // consume alignof
        if (!expect(TokenType::TOKEN_LPAREN, "expected '(' after alignof")) {
            return nullptr;
        }
        if (!isTypeStart() && !check(TokenType::TOKEN_CONST) &&
            !check(TokenType::TOKEN_VOLATILE) && !check(TokenType::TOKEN_ATOMIC)) {
            errorUnexpected("expected type after 'alignof('");
            return nullptr;
        }
        Type* ty = parseTypeId();
        if (!ty) return nullptr;
        if (!expect(TokenType::TOKEN_RPAREN, "expected ')' after alignof type")) {
            return nullptr;
        }
        auto align = std::make_unique<SizeofExprAST>(ty);
        align->isAlignOf = true;
        return align;
    }

    // PAR-14: `offsetof(T, member)`.
    if (token->type == TokenType::TOKEN_OFFSETOF) {
        advance(); // consume offsetof
        if (!expect(TokenType::TOKEN_LPAREN, "expected '(' after offsetof")) {
            return nullptr;
        }
        if (!isTypeStart() && !check(TokenType::TOKEN_CONST) &&
            !check(TokenType::TOKEN_VOLATILE)) {
            errorUnexpected("expected type after 'offsetof('");
            return nullptr;
        }
        Type* ty = parseType();
        if (!ty) return nullptr;
        if (!expect(TokenType::TOKEN_COMMA, "expected ',' in offsetof")) {
            return nullptr;
        }
        auto nameTok = match(TokenType::TOKEN_IDENTIFIER);
        if (!nameTok) {
            errorUnexpected("expected member name in offsetof");
            return nullptr;
        }
        std::vector<OffsetDesignator> path;
        { OffsetDesignator d; d.field = nameTok->lexeme; path.push_back(std::move(d)); }
        while (!eof()) {
            if (match(TokenType::TOKEN_DOT)) {
                auto sub = match(TokenType::TOKEN_IDENTIFIER);
                if (!sub) {
                    errorUnexpected("expected member name after '.' in offsetof");
                    return nullptr;
                }
                OffsetDesignator d; d.field = sub->lexeme; path.push_back(std::move(d));
            } else if (match(TokenType::TOKEN_LBRACKET)) {
                auto indexExpr = parseExpr(2);
                if (!indexExpr) {
                    errorUnexpected("expected constant expression in offsetof subscript");
                    return nullptr;
                }
                OffsetDesignator d;
                d.isIndex = true;
                d.indexExpr = std::move(indexExpr);
                path.push_back(std::move(d));
                if (!expect(TokenType::TOKEN_RBRACKET, "expected ']' in offsetof")) return nullptr;
            } else {
                break;
            }
        }
        if (!expect(TokenType::TOKEN_RPAREN, "expected ')' after offsetof")) {
            return nullptr;
        }
        auto node = std::make_unique<OffsetOfExprAST>(ty, nameTok->lexeme);
        node->path = std::move(path);
        return node;
    }

    // Handle prefix unary operators
    if (isUnaryOp(token->type)) {
        auto op = token->type;
        advance(); // consume operator
        auto operand = parseUnary();
        if (!operand) {
            return nullptr;
        }
        // 解析优先级缺陷轮：后缀（`.`/`->`/`[...]`/调用）绑定必须紧于前缀
        // 一元——`-a.v` 是 `-(a.v)` 而非 `(-a).v`，`-arr[i]` 是 `-(arr[i])`。
        // 入口处的 parsePostfix 只作用于最外层，这里补内层。
        operand = parsePostfix(std::move(operand));
        auto unaryOp = tokenToUnaryOp(op);
        return std::make_unique<UnaryExprAST>(unaryOp, std::move(operand));
    }

    return parsePrimary();
}

// P1-08 / PAR-16: `asm [volatile] ("tpl" [: outputs [: inputs [: clobbers]]])`.
// `asm` 已由 parseUnaryImpl 消费。至多三段操作数，段可为空；模板必须是字符串
// 字面量（GCC 同样要求），取其去引号并解码后的文本。
std::unique_ptr<ExprAST> Parser::parseAsmExpr() {
    const bool isVolatile = match(TokenType::TOKEN_VOLATILE).has_value();
    if (!expect(TokenType::TOKEN_LPAREN, "expected '(' after asm")) return nullptr;

    if (!check(TokenType::TOKEN_STRING)) {
        errorUnexpected("string literal asm template");
        return nullptr;
    }
    auto tplTok = advance();
    const std::string lex = tplTok ? tplTok->lexeme : std::string();
    // 字符串 token 的 lexeme 含两端引号；模板/约束/clobber 都取其值文本
    // （GCC 语义：`"=r"` 的值是 `=r`，`"memory"` 的值是 `memory`）。
    auto strValue = [](const std::string& s) {
        return s.size() >= 2 && s.front() == '"' && s.back() == '"'
                   ? decodeEscapes(s.substr(1, s.size() - 2))
                   : s;
    };
    std::string asmTemplate = strValue(lex);

    std::vector<AsmOperand> outputs;
    std::vector<AsmOperand> inputs;
    std::vector<std::string> clobbers;
    int section = 0; // 0=template, 1=outputs, 2=inputs, 3=clobbers

    while (true) {
        if (check(TokenType::TOKEN_RPAREN)) break;
        // GCC allows adjacent colons for empty sections (`:::` / `::`). The
        // lexer folds `::` into TOKEN_COLON_COLON (for qualified names), so
        // count it as two separators here.
        if (check(TokenType::TOKEN_COLON_COLON)) {
            advance();
            section += 2;
            if (section > 3) {
                errorUnexpected("at most three asm operand sections");
                return nullptr;
            }
            continue;
        }
        if (check(TokenType::TOKEN_COLON)) {
            advance();
            ++section;
            if (section > 3) {
                errorUnexpected("at most three asm operand sections");
                return nullptr;
            }
            continue;
        }
        if (section == 0) {
            errorUnexpected("':' or ')' after asm template");
            return nullptr;
        }

        if (section == 3) {
            // clobbers: 逗号分隔的字符串列表。
            do {
                if (!check(TokenType::TOKEN_STRING)) {
                    errorUnexpected("string literal asm clobber");
                    return nullptr;
                }
                auto tok = advance();
                clobbers.push_back(strValue(tok ? tok->lexeme : std::string()));
            } while (match(TokenType::TOKEN_COMMA));
            continue;
        }

        // outputs / inputs: `[name] "constraint"(expr)` 逗号分隔列表。
        do {
            AsmOperand op;
            if (match(TokenType::TOKEN_LBRACKET)) {
                if (!check(TokenType::TOKEN_IDENTIFIER)) {
                    errorUnexpected("expected asm operand name");
                    return nullptr;
                }
                op.name = advance()->lexeme;
                if (!expect(TokenType::TOKEN_RBRACKET,
                            "expected ']' after asm operand name")) {
                    return nullptr;
                }
            }
            if (!check(TokenType::TOKEN_STRING)) {
                errorUnexpected("string literal asm constraint");
                return nullptr;
            }
            auto tok = advance();
            op.constraint = strValue(tok ? tok->lexeme : std::string());
            if (!expect(TokenType::TOKEN_LPAREN, "expected '(' after asm constraint")) return nullptr;
            op.expr = parseExpr();
            if (!op.expr) {
                errorUnexpected("expression in asm operand");
                return nullptr;
            }
            if (!expect(TokenType::TOKEN_RPAREN, "expected ')' after asm operand")) return nullptr;
            if (section == 1) outputs.push_back(std::move(op));
            else inputs.push_back(std::move(op));
        } while (match(TokenType::TOKEN_COMMA));
    }

    if (!expect(TokenType::TOKEN_RPAREN, "expected ')' to close asm")) return nullptr;
    return std::make_unique<AsmExprAST>(std::move(asmTemplate), isVolatile,
                                        std::move(outputs), std::move(inputs),
                                        std::move(clobbers));
}

// PAR-11: lambda 表达式 `[captures](params) -> Ret { body }`。
// 调用点：parsePrimaryImpl 遇 `[`（表达式起始位置，无下标歧义）。
std::unique_ptr<ExprAST> Parser::parseLambdaExpr() {
    if (!match(TokenType::TOKEN_LBRACKET)) return nullptr;
    auto lambda = std::make_unique<LambdaExprAST>();

    // 显式捕获项：可选 `&`，identifier，可选 `= expr`。捕获初始化器用
    // minPrec=2 解析，避免逗号被当作逗号运算符吃掉列表分隔符。
    auto parseCaptureItem = [this](std::vector<LambdaExprAST::Capture>& out,
                                   bool leadingRefConsumed = false) -> bool {
        LambdaExprAST::Capture cap;
        if (leadingRefConsumed) {
            cap.byRef = true;
        } else if (match(TokenType::TOKEN_BIT_AND)) {
            cap.byRef = true;
        }
        auto name = match(TokenType::TOKEN_IDENTIFIER);
        if (!name) {
            errorUnexpected("expected capture name or ']' in lambda capture list");
            return false;
        }
        cap.name = name->lexeme;
        if (match(TokenType::TOKEN_ASSIGN)) {
            cap.init = parseExpr(2);
            if (!cap.init) {
                errorUnexpected("capture initializer expression");
                return false;
            }
        }
        out.push_back(std::move(cap));
        return true;
    };

    auto parseCaptureList = [&]() -> bool {
        while (true) {
            if (!parseCaptureItem(lambda->captures)) return false;
            if (match(TokenType::TOKEN_COMMA)) continue;
            return expect(TokenType::TOKEN_RBRACKET,
                          "expected ']' after lambda capture list");
        }
    };

    // capture list:
    //   `]` | `[=]` | `[&]`
    //   `[=, a, &b]` | `[&, a, &b]`（默认捕获 + override 列表）
    //   `[a, &b]` | `[x = expr, &y = expr]` | `[this]`
    if (match(TokenType::TOKEN_RBRACKET)) {
        // empty capture
    } else if (match(TokenType::TOKEN_ASSIGN)) {
        lambda->captureAll = true;
        if (match(TokenType::TOKEN_COMMA)) {
            if (!parseCaptureList()) return nullptr;
        } else if (!expect(TokenType::TOKEN_RBRACKET,
                           "expected ']' after '=' in lambda capture list")) {
            return nullptr;
        }
    } else if (check(TokenType::TOKEN_BIT_AND)) {
        advance();  // consume '&'；需区分 `[&]`/`[&, ...]` 和显式 `[&name]`。
        if (match(TokenType::TOKEN_RBRACKET)) {
            lambda->captureAll = true;
            lambda->captureAllByRef = true;
        } else if (match(TokenType::TOKEN_COMMA)) {
            lambda->captureAll = true;
            lambda->captureAllByRef = true;
            if (!parseCaptureList()) return nullptr;
        } else {
            // 显式 `&name` / `&name = expr`（`&` 已消费）。
            if (!parseCaptureItem(lambda->captures, true)) return nullptr;
            if (match(TokenType::TOKEN_COMMA)) {
                if (!parseCaptureList()) return nullptr;
            } else if (!expect(TokenType::TOKEN_RBRACKET,
                               "expected ']' after lambda capture list")) {
                return nullptr;
            }
        }
    } else {
        if (!parseCaptureList()) return nullptr;
    }

    // (params) — reuse function parameter parsing.
    if (!expect(TokenType::TOKEN_LPAREN, "expected '(' after lambda capture list")) return nullptr;
    if (!check(TokenType::TOKEN_RPAREN)) {
        while (true) {
            auto param = parseParamDecl();
            if (!param) {
                errorUnexpected("lambda parameter");
                return nullptr;
            }
            lambda->params.push_back(std::move(param));
            if (match(TokenType::TOKEN_COMMA)) continue;
            break;
        }
    }
    if (!expect(TokenType::TOKEN_RPAREN, "expected ')' after lambda parameters")) return nullptr;

    // optional -> Ret
    if (match(TokenType::TOKEN_ARROW)) {
        lambda->returnType = parseType();
        if (!lambda->returnType) {
            errorUnexpected("lambda return type after '->'");
            return nullptr;
        }
        lambda->hasExplicitReturnType = true;
    }

    // { body }
    if (!check(TokenType::TOKEN_LBRACE)) {
        errorUnexpected("expected '{' to begin lambda body");
        return nullptr;
    }
    lambda->body = parseCompoundStmt();

    return lambda;
}

std::unique_ptr<ExprAST> Parser::parsePrimary() {
    auto startTok = peek();
    auto expr = parsePrimaryImpl();
    if (expr) {
        applyLocation(expr.get(), startTok ? &*startTok : nullptr);
    }
    return expr;
}

std::unique_ptr<ExprAST> Parser::parsePrimaryImpl() {
    auto token = peek();
    if (!token) {
        return nullptr;
    }

    // Number literal
    if (token->type == TokenType::TOKEN_NUMBER) {
        advance();
        // LEX-15/17: carry the full 128-bit value (low + high words) and the
        // literal kind into the AST. ≤64-bit literals have hi == 0.
        unsigned __int128 v = std::get<unsigned __int128>(token->value);
        long long lo = static_cast<long long>(v);
        long long hi = static_cast<long long>(v >> 64);
        return std::make_unique<NumberExprAST>(lo, hi, token->literalKind);
    }

    // Float literal
    if (token->type == TokenType::TOKEN_FLOAT) {
        advance();
        return std::make_unique<FloatExprAST>(std::get<double>(token->value),
                                              token->literalKind);
    }

    // Char literal
    if (token->type == TokenType::TOKEN_CHAR) {
        advance();
        std::string lex = token->lexeme;
        char val = 0;
        // SG-18: C character prefixes (`L'a'`, `u'a'`, `U'a'`, `u8'a'`).
        const size_t openQuote = lex.find('\'');
        const size_t closeQuote = lex.rfind('\'');
        if (openQuote != std::string::npos && closeQuote > openQuote) {
            std::string decoded = decodeEscapes(
                lex.substr(openQuote + 1, closeQuote - openQuote - 1));
            if (!decoded.empty()) val = decoded[0];
        }
        return std::make_unique<CharExprAST>(val);
    }

    // String literal (SG-07: adjacent literals concatenate).
    if (token->type == TokenType::TOKEN_STRING) {
        auto first = advance();
        StringLiteralPrefix prefix = first->stringPrefix;
        std::string val = decodeStringToken(*first, prefix);
        while (check(TokenType::TOKEN_STRING)) {
            auto next = advance();
            StringLiteralPrefix partPrefix = next->stringPrefix;
            std::string part = decodeStringToken(*next, partPrefix);
            if (prefix == StringLiteralPrefix::None &&
                partPrefix != StringLiteralPrefix::None) {
                prefix = partPrefix;
            }
            val += part;
        }
        if (prefix == StringLiteralPrefix::None) {
            return std::make_unique<StringExprAST>(val);
        }
        return std::make_unique<StringExprAST>(val, prefix);
    }

    // Boolean literals (found issue: 布尔字面量，非 int 字面量——bool 类型)。
    if (token->type == TokenType::TOKEN_TRUE) {
        advance();
        return std::make_unique<BoolLiteralExprAST>(true);
    }
    if (token->type == TokenType::TOKEN_FALSE) {
        advance();
        return std::make_unique<BoolLiteralExprAST>(false);
    }
    // null literal
    if (token->type == TokenType::TOKEN_NULL) {
        advance();
        return std::make_unique<NumberExprAST>(0);
    }

    // Identifier or function call
    if (token->type == TokenType::TOKEN_IDENTIFIER) {
        std::string name = token->lexeme;
        advance();

        // Qualified name: A::B::name (namespace member access).
        while (check(TokenType::TOKEN_COLON_COLON)) {
            advance(); // consume '::'
            auto part = match(TokenType::TOKEN_IDENTIFIER);
            if (!part) {
                errorUnexpected("expected identifier after '::'");
                break;
            }
            name += "::" + part->lexeme;
        }

        // P1-03 / GEN-03: `name<args>(...)` 显式模板调用——投机解析实参列
        // 表且必须紧跟 `(`；失败回滚按比较表达式处理（`a < b > (c)` 歧义
        // 取比较，C++ 同款立场）。
        std::vector<Type*> explicitTemplateArgs;
        std::vector<long long> explicitTemplateValues;
        std::vector<std::string> explicitTemplateValueNames;
        std::vector<std::shared_ptr<ExprAST>> explicitTemplateValueExprs;
        bool hasTemplateArgs = false;
        // 评审 C2：closeBracket 会原地拆分 RSHIFT token——回滚须恢复，
        // 否则比较表达式被静默错译。
        std::vector<std::pair<size_t, TokenType>> rshiftMutations;
        if (peek() && peek()->type == TokenType::TOKEN_LT) {
            size_t save = m_currentTokenPos;
            bool ok = true;
            advance(); // '<'
            auto closeBracket = [this, &rshiftMutations]() -> bool {
                if (check(TokenType::TOKEN_GT)) {
                    advance();
                    return true;
                }
                if (check(TokenType::TOKEN_RSHIFT)) {
                    rshiftMutations.emplace_back(m_currentTokenPos, TokenType::TOKEN_RSHIFT);
                    m_tokens[m_currentTokenPos].type = TokenType::TOKEN_GT;
                    m_tokens[m_currentTokenPos].lexeme = ">";
                    return true;
                }
                return false;
            };
            if (check(TokenType::TOKEN_GT) || check(TokenType::TOKEN_RSHIFT)) {
                ok = closeBracket(); // `name<>()` 空实参：纯推导
            } else {
                m_templateArgSpeculative = true;
                while (true) {
                    Type* argType = nullptr;
                    long long argValue = 0;
                    std::string argName;
                    std::shared_ptr<ExprAST> argExpr;
                    if (!parseTemplateArgument(argType, argValue, argName, argExpr)) {
                        ok = false;
                        break;
                    }
                    if (argType) {
                        explicitTemplateArgs.push_back(argType);
                    } else {
                        explicitTemplateValues.push_back(argValue);
                        explicitTemplateValueNames.push_back(std::move(argName));
                        explicitTemplateValueExprs.push_back(std::move(argExpr));
                    }
                    if (match(TokenType::TOKEN_COMMA)) continue;
                    break;
                }
                m_templateArgSpeculative = false;
                if (ok) ok = closeBracket();
            }
            if (ok) ok = peek() && peek()->type == TokenType::TOKEN_LPAREN;
            if (ok) {
                hasTemplateArgs = true;
            } else {
                // 评审 C2：恢复被 closeBracket 拆分的 RSHIFT token。
                for (auto& [pos, orig] : rshiftMutations) {
                    m_tokens[pos].type = orig;
                    m_tokens[pos].lexeme = ">>";
                }
                m_currentTokenPos = save;
                explicitTemplateArgs.clear();
                explicitTemplateValues.clear();
                explicitTemplateValueNames.clear();
                explicitTemplateValueExprs.clear();
            }
        }

        // Function call: identifier(args)
        if (peek() && peek()->type == TokenType::TOKEN_LPAREN) {
            advance(); // consume '('
            std::vector<std::unique_ptr<ExprAST>> args;

            if (!peek() || peek()->type != TokenType::TOKEN_RPAREN) {
                while (true) {
                    auto arg = parseExpr(2); // minPrec=2 to avoid consuming comma operator
                    if (!arg) {
                        return nullptr;
                    }
                    args.push_back(std::move(arg));

                    if (!match(TokenType::TOKEN_COMMA)) {
                        break;
                    }
                }
            }

            expect(TokenType::TOKEN_RPAREN, "expected ')' after function arguments");
            auto call = std::make_unique<CallExprAST>(name, std::move(args));
            call->hasTemplateArgs = hasTemplateArgs;
            call->explicitTemplateArgs = std::move(explicitTemplateArgs);
            call->explicitTemplateValues = std::move(explicitTemplateValues);
            call->explicitTemplateValueNames = std::move(explicitTemplateValueNames);
            call->explicitTemplateValueExprs = std::move(explicitTemplateValueExprs);
            return call;
        }

        return std::make_unique<VariableExprAST>(name);
    }

    // PAR-11: lambda 表达式 `[captures](params) -> Ret { body }`。
    // 表达式起始位置的 `[` 无下标含义（下标是后缀），安全识别为 lambda。
    if (token->type == TokenType::TOKEN_LBRACKET) {
        if (auto lambda = parseLambdaExpr()) {
            return lambda;
        }
        errorUnexpected("expected lambda capture list or array subscript");
        return nullptr;
    }

    // Parenthesized expression
    if (token->type == TokenType::TOKEN_LPAREN) {
        advance();
        auto expr = parseExpr();
        expect(TokenType::TOKEN_RPAREN, "expected ')' after expression");
        return expr;
    }

    // Initializer list: {expr, expr, ...} — PAR-13: 支持 C99 指定初始化器
    // 设计符（.field / [idx] 链），designators 与元素平行存储。
    if (token->type == TokenType::TOKEN_LBRACE) {
        advance();
        std::vector<std::unique_ptr<ExprAST>> initList;
        std::vector<std::vector<Designator>> designatorList;

        if (!check(TokenType::TOKEN_RBRACE)) {
            while (true) {
                // 设计符链：仅 `{` 内元素前识别（`.`/`[` 不是表达式起始，无歧义）。
                std::vector<Designator> designators;
                while (check(TokenType::TOKEN_DOT) || check(TokenType::TOKEN_LBRACKET)) {
                    Designator d;
                    if (match(TokenType::TOKEN_DOT)) {
                        d.kind = Designator::Kind::Field;
                        auto id = match(TokenType::TOKEN_IDENTIFIER);
                        if (!id) {
                            errorUnexpected("expected field name after '.' in designated initializer");
                            return nullptr;
                        }
                        d.field = id->lexeme;
                    } else {
                        advance(); // '['
                        d.kind = Designator::Kind::Index;
                        d.indexExpr = parseExpr(); // 常量表达式，sema 折叠
                        if (!d.indexExpr) return nullptr;
                        if (!expect(TokenType::TOKEN_RBRACKET,
                                    "expected ']' after designator index")) {
                            return nullptr;
                        }
                    }
                    designators.push_back(std::move(d));
                }
                if (!designators.empty()) {
                    // C99: 设计符后须 '='（省略值语法 `.x,` 不做，YAGNI）。
                    if (!match(TokenType::TOKEN_ASSIGN)) {
                        errorUnexpected("expected '=' after designator");
                        return nullptr;
                    }
                }
                auto expr = parseExpr(2); // minPrec=2: ',' separates elements
                if (expr) {
                    initList.push_back(std::move(expr));
                    designatorList.push_back(std::move(designators));
                }
                if (!match(TokenType::TOKEN_COMMA)) break;
            }
        }

        expect(TokenType::TOKEN_RBRACE, "expected '}' to end initializer list");
        auto node = std::make_unique<InitializerListExprAST>(std::move(initList));
        node->designators = std::move(designatorList);
        return node;
    }

    errorUnexpected("expected expression");
    return nullptr;
}

std::unique_ptr<ExprAST> Parser::parsePostfix(std::unique_ptr<ExprAST> lhs) {
    if (!lhs) {
        return nullptr;
    }

    while (auto token = peek()) {
        switch (token->type) {
            case TokenType::TOKEN_PLUS_PLUS:
            case TokenType::TOKEN_MINUS_MINUS: {
                bool isInc = (token->type == TokenType::TOKEN_PLUS_PLUS);
                advance();
                lhs = std::make_unique<PostfixIncDecExprAST>(std::move(lhs), isInc);
                break;
            }
            case TokenType::TOKEN_LPAREN: {
                advance(); // consume '('
                std::vector<std::unique_ptr<ExprAST>> args;
                // P1-04 / CT-06 + CTX-01: compile_time queries take type
                // arguments. Built-in scalar types are keywords, so parseExpr
                // cannot handle them; parse them as type-ids and wrap them in a
                // SizeofExprAST carrier. The per-member policy decides whether
                // every argument or only the first may be a type.
                CTArgPolicy ctPolicy = CTArgPolicy::None;
                if (auto* memberAccess = dynamic_cast<MemberAccessExprAST*>(lhs.get())) {
                    if (isCompileTimeChainRoot(memberAccess->object.get())) {
                        ctPolicy = ctArgPolicyFor(memberAccess->memberName);
                    }
                }
                if (!peek() || peek()->type != TokenType::TOKEN_RPAREN) {
                    bool firstArg = true;
                    while (true) {
                        std::unique_ptr<ExprAST> arg;
                        bool tryType = false;
                        if (ctPolicy == CTArgPolicy::AllTypes) {
                            tryType = isTypeStart();
                        } else if (ctPolicy == CTArgPolicy::FirstType && firstArg) {
                            tryType = isTypeStart();
                        }
                        if (tryType) {
                            Type* ty = parseType();
                            if (!ty) return nullptr;
                            arg = std::make_unique<SizeofExprAST>(ty, nullptr);
                        } else {
                            arg = parseExpr(2); // minPrec=2: ',' separates args
                        }
                        if (!arg) return nullptr;
                        args.push_back(std::move(arg));
                        firstArg = false;
                        if (!match(TokenType::TOKEN_COMMA)) break;
                    }
                }
                expect(TokenType::TOKEN_RPAREN, "expected ')' after function arguments");
                // For member access expressions, create a method call
                if (auto* memberAccess = dynamic_cast<MemberAccessExprAST*>(lhs.get())) {
                    auto object = std::move(memberAccess->object);
                    lhs = std::make_unique<MethodCallExprAST>(
                        std::move(object), memberAccess->memberName, std::move(args));
                } else if (auto* lam = dynamic_cast<LambdaExprAST*>(lhs.get())) {
                    // PAR-11: 立即调用 `[...](...){} (args)`。
                    auto lamPtr = std::unique_ptr<LambdaExprAST>(
                        static_cast<LambdaExprAST*>(lhs.release()));
                    lhs = std::make_unique<LambdaCallExprAST>(std::move(lamPtr), std::move(args));
                } else {
                    // General expression callee: `arr[0](1)`, `get()(1)`.
                    auto call = std::make_unique<CallExprAST>("", std::move(args));
                    call->calleeExpr = std::move(lhs);
                    call->isExprCallee = true;
                    lhs = std::move(call);
                }
                break;
            }
            case TokenType::TOKEN_LBRACKET: {
                advance(); // consume '['
                // Review 2.2: `s[a:b]`, `s[:b]`, `s[a:]`, `s[:]`.
                if (match(TokenType::TOKEN_COLON)) {
                    std::unique_ptr<ExprAST> end;
                    if (!check(TokenType::TOKEN_RBRACKET)) {
                        end = parseExpr();
                        if (!end) return nullptr;
                    }
                    expect(TokenType::TOKEN_RBRACKET, "expected ']' after range");
                    lhs = std::make_unique<RangeExprAST>(
                        std::move(lhs), nullptr, std::move(end));
                    break;
                }
                auto index = parseExpr();
                if (!index) return nullptr;
                if (match(TokenType::TOKEN_COLON)) {
                    std::unique_ptr<ExprAST> end;
                    if (!check(TokenType::TOKEN_RBRACKET)) {
                        end = parseExpr();
                        if (!end) return nullptr;
                    }
                    expect(TokenType::TOKEN_RBRACKET, "expected ']' after range");
                    lhs = std::make_unique<RangeExprAST>(
                        std::move(lhs), std::move(index), std::move(end));
                    break;
                }
                expect(TokenType::TOKEN_RBRACKET, "expected ']' after array index");
                lhs = std::make_unique<ArrayAccessExprAST>(std::move(lhs), std::move(index));
                break;
            }
            case TokenType::TOKEN_DOT: {
                advance(); // consume '.'
                if (auto member = match(TokenType::TOKEN_IDENTIFIER)) {
                    auto node = std::make_unique<MemberAccessExprAST>(
                        MemberAccessKind::Dot, std::move(lhs), member->lexeme);
                    applyLocation(node.get(), &*member);
                    lhs = std::move(node);
                } else {
                    errorUnexpected("expected member name after '.'");
                }
                break;
            }
            case TokenType::TOKEN_ARROW: {
                advance(); // consume '->'
                if (auto member = match(TokenType::TOKEN_IDENTIFIER)) {
                    auto node = std::make_unique<MemberAccessExprAST>(
                        MemberAccessKind::Arrow, std::move(lhs), member->lexeme);
                    applyLocation(node.get(), &*member);
                    lhs = std::move(node);
                } else {
                    errorUnexpected("expected member name after '->'");
                }
                break;
            }
            default:
                return lhs;
        }
    }

    return lhs;
}

std::unique_ptr<ExprAST> Parser::parseExpr(int minPrec) {
    auto token = peek();
    if (!token) {
        return nullptr;
    }
    // The first token of this expression; every node built at this precedence
    // level is stamped with it so diagnostics can point at the expression
    // (INF-03 / P0-06). Sub-expressions are stamped by their own recursive call.
    const Token* startTok = &*token;

    std::unique_ptr<ExprAST> lhs;

    lhs = parseUnary();

    if (!lhs) {
        return nullptr;
    }
    applyLocation(lhs.get(), startTok);

    lhs = parsePostfix(std::move(lhs));
    applyLocation(lhs.get(), startTok);

    // Pratt climbing loop
    while (!eof()) {
        auto opToken = peek();
        if (!opToken) break;

        // SG-01: inside `<...>` a top-level `>` closes the template
        // argument list rather than acting as a comparison operator.
        if (m_templateArgDepth > 0 &&
            (opToken->type == TokenType::TOKEN_GT ||
             opToken->type == TokenType::TOKEN_RSHIFT)) {
            break;
        }
        int prec = getPrecedence(opToken->type);
        if (prec < minPrec) break;

        // Comma operator: lowest precedence, left-associative, represented by
        // its own AST node (CommaExprAST).
        if (opToken->type == TokenType::TOKEN_COMMA) {
            advance(); // consume ','
            auto rhs = parseExpr(prec + 1); // left-associative
            if (!rhs) return nullptr;
            lhs = std::make_unique<CommaExprAST>(std::move(lhs), std::move(rhs));
            applyLocation(lhs.get(), startTok);
            continue;
        }

        // Handle ternary operator specially (right-to-left, needs ? and :)
        if (opToken->type == TokenType::TOKEN_QUESTION) {
            advance(); // consume '?'
            auto thenExpr = parseExpr(); // then-branch is a full expression (may contain ',')
            if (!thenExpr) return nullptr;
            match(TokenType::TOKEN_COLON);
            auto elseExpr = parseExpr(2); // else-branch: ',' separates, not an operator
            if (!elseExpr) return nullptr;
            lhs = std::make_unique<TernaryExprAST>(std::move(lhs), std::move(thenExpr), std::move(elseExpr));
            applyLocation(lhs.get(), startTok);
            continue;
        }

        // Handle assignment operators (right-to-left)
        if (opToken->type == TokenType::TOKEN_ASSIGN ||
            opToken->type == TokenType::TOKEN_PLUS_EQ ||
            opToken->type == TokenType::TOKEN_MINUS_EQ ||
            opToken->type == TokenType::TOKEN_STAR_EQ ||
            opToken->type == TokenType::TOKEN_SLASH_EQ ||
            opToken->type == TokenType::TOKEN_PERCENT_EQ ||
            opToken->type == TokenType::TOKEN_AMP_EQ ||
            opToken->type == TokenType::TOKEN_PIPE_EQ ||
            opToken->type == TokenType::TOKEN_CARET_EQ ||
            opToken->type == TokenType::TOKEN_LSHIFT_EQ ||
            opToken->type == TokenType::TOKEN_RSHIFT_EQ) {
            auto assignOp = tokenTypeToAssignOp(opToken->type);
            advance(); // consume assignment operator
            auto rhs = parseExpr(prec); // right-to-left: use same prec
            if (!rhs) return nullptr;
            lhs = std::make_unique<AssignmentExprAST>(assignOp, std::move(lhs), std::move(rhs));
            applyLocation(lhs.get(), startTok);
            continue;
        }

        // Binary operators
        if (prec > 0) {
            auto binOp = tokenTypeToBinaryOp(opToken->type);
            if (binOp == BinaryOp::Invalid) {
                break;
            }
            advance(); // consume operator
            int nextPrec = isRightAssociative(opToken->type) ? prec : prec + 1;
            auto rhs = parseExpr(nextPrec);
            if (!rhs) return nullptr;
            lhs = std::make_unique<BinaryExprAST>(binOp, std::move(lhs), std::move(rhs));
            applyLocation(lhs.get(), startTok);
            continue;
        }

        break;
    }

    return lhs;
}

// ========== Declaration Parsing ==========

std::unique_ptr<ReturnStmtAST> Parser::parseReturnStmt() {
    auto retToken = match(TokenType::TOKEN_RETURN);
    if (!retToken) {
        return nullptr;
    }

    // 允许裸 `return;`（void 函数早退）——sema/codegen 已就绪（void 检查 +
    // CreateRetVoid），仅解析器硬性要求表达式。
    std::unique_ptr<ExprAST> value;
    if (peek() && peek()->type != TokenType::TOKEN_SEMICOLON) {
        value = parseExpr();
        if (!value) {
            errorUnexpected("expected expression after 'return'");
            return nullptr;
        }
    }

    expect(TokenType::TOKEN_SEMICOLON, "expected ';' after return statement");
    auto stmt = std::make_unique<ReturnStmtAST>(std::move(value));
    stmt->setLocation(retToken->filename, retToken->line, retToken->column);
    return stmt;
}

std::unique_ptr<CompoundStmtAST> Parser::parseCompoundStmt() {
    if (!expect(TokenType::TOKEN_LBRACE, "expected '{' to begin compound statement")) {
        return nullptr;
    }

    TypeContext::instance().pushLocalTypedefScope();
    m_constScopes.emplace_back();
    std::vector<std::unique_ptr<StmtAST>> stmts{};
    while(!eof() && peek()->type != TokenType::TOKEN_RBRACE) {
        auto stmt = parseStmt();
        if (!stmt) {
            advance();
            continue;
        }
        stmts.push_back(std::move(stmt));
    }
    expect(TokenType::TOKEN_RBRACE, "expected '}' to end compound statement");
    m_constScopes.pop_back();
    TypeContext::instance().popLocalTypedefScope();

    return std::make_unique<CompoundStmtAST>(std::move(stmts));
}

bool Parser::rejectPreprocessorDirective() {
    if (!check(TokenType::TOKEN_HASH)) return false;
    auto hash = advance();
    error("preprocessor directives are not supported (Cmm has no preprocessor)", *hash);
    // Skip the remainder of the directive line to avoid cascading errors.
    while (peek() && peek()->type != TokenType::TOKEN_EOS && peek()->line == hash->line) {
        advance();
    }
    return true;
}

std::unique_ptr<StmtAST> Parser::parseStmt() {
    auto stmtStart = peek();
    if (rejectPreprocessorDirective()) {
        return std::make_unique<NullStmtAST>();
    }

    // P1-05 / ANN-01: 语句位置注解——仅声明语句合法（其余语句 E1002 兜底）。
    std::vector<Annotation> stmtAnns;
    if (isAnnotationStart()) {
        stmtAnns = parseAnnotations();
        if (!(isTypeStart() || check(TokenType::TOKEN_CONST) ||
              check(TokenType::TOKEN_VOLATILE) || check(TokenType::TOKEN_CONSTEXPR) ||
              check(TokenType::TOKEN_ATOMIC) || isCompileTimeModifierStart() ||
              isLocalUsingStart())) {
            errorUnexpected("annotations are only allowed on declarations");
            return nullptr;
        }
    }

    // C null statement: a lone ';' is a valid statement (and the natural
    // empty body for `while (...) ;` / `for (...) ;`).
    if (auto semi = match(TokenType::TOKEN_SEMICOLON)) {
        auto stmt = std::make_unique<NullStmtAST>();
        stmt->setLocation(semi->filename, semi->line, semi->column);
        return stmt;
    }

    // Stamp declaration statements with the location of their first token
    // (INF-03 / P0-06), so sema diagnostics on the statement carry a position.
    auto makeDeclStmt = [&](std::unique_ptr<DeclAST> decl) {
        if (!stmtAnns.empty()) {
            decl->annotations = std::move(stmtAnns);
        }
        auto stmt = std::make_unique<DeclStmtAST>(std::move(decl));
        if (stmtStart) {
            stmt->setLocation(stmtStart->filename, stmtStart->line, stmtStart->column);
        }
        return stmt;
    };

    // PAR-24: block-scoped alias declaration (`using T = type-id;`).
    if (isLocalUsingStart()) {
        auto u = parseUsingAliasDecl(/*member=*/false);
        if (!u) return nullptr;
        return makeDeclStmt(std::move(u));
    }

    // if statement
    if (peek() && peek()->type == TokenType::TOKEN_IF) {
        return parseIfStmt();
    }

    // while statement
    if (peek() && peek()->type == TokenType::TOKEN_WHILE) {
        return parseWhileStmt();
    }

    // do-while statement
    if (peek() && peek()->type == TokenType::TOKEN_DO) {
        return parseDoWhileStmt();
    }

    // for statement
    if (peek() && peek()->type == TokenType::TOKEN_FOR) {
        return parseForStmt();
    }

    // switch statement
    if (peek() && peek()->type == TokenType::TOKEN_SWITCH) {
        return parseSwitchStmt();
    }

    // break statement
    if (peek() && peek()->type == TokenType::TOKEN_BREAK) {
        return parseBreakStmt();
    }

    // continue statement
    if (peek() && peek()->type == TokenType::TOKEN_CONTINUE) {
        return parseContinueStmt();
    }

    // return statement
    if (auto retToken = match(TokenType::TOKEN_RETURN)) {
        // 允许裸 `return;`（void 函数早退）。
        std::unique_ptr<ExprAST> value;
        if (peek() && peek()->type != TokenType::TOKEN_SEMICOLON) {
            value = parseExpr();
            if (!value) {
                errorUnexpected("expected expression after 'return'");
                return nullptr;
            }
        }

        expect(TokenType::TOKEN_SEMICOLON, "expected ';' after return statement");
        auto stmt = std::make_unique<ReturnStmtAST>(std::move(value));
        stmt->setLocation(retToken->filename, retToken->line, retToken->column);
        return stmt;
    }

    // defer statement
    if (peek() && peek()->type == TokenType::TOKEN_DEFER) {
        advance(); // 消耗 "defer"
        auto callExpr = parseExpr();
        if (!callExpr) {
            errorUnexpected("expected expression after 'defer'");
            return nullptr;
        }
        expect(TokenType::TOKEN_SEMICOLON, "expected ';' after defer statement");
        auto stmt = std::make_unique<DeferStmtAST>(std::move(callExpr));
        if (stmtStart) {
            stmt->setLocation(stmtStart->filename, stmtStart->line, stmtStart->column);
        }
        return stmt;
    }

    // compound statement (block)
    if (peek() && peek()->type == TokenType::TOKEN_LBRACE) {
        return parseCompoundStmt();
    }

    // Function-body compile_time.if — statement form.
    if (peek() && peek()->type == TokenType::TOKEN_IDENTIFIER &&
        peek()->lexeme == "compile_time" && m_currentTokenPos + 2 < m_tokens.size() &&
        m_tokens[m_currentTokenPos + 1].type == TokenType::TOKEN_DOT) {
        const Token& member = m_tokens[m_currentTokenPos + 2];
        if (member.type == TokenType::TOKEN_IF) {
            if (auto ctIf = parseCompileTimeIfStmt()) return ctIf;
        } else if (member.type == TokenType::TOKEN_IDENTIFIER &&
                   member.lexeme == "repeat") {
            if (auto ctRepeat = parseCompileTimeRepeatStmt()) return ctRepeat;
        }
    }

    // Local record declarations: `struct S { ... };`, `union U { ... };`,
    // `enum E { ... };` — route through the normal declaration parser (the
    // sema/codegen already handle record decls in DeclStmtAST).
    if (check(TokenType::TOKEN_STRUCT) || check(TokenType::TOKEN_UNION) ||
        check(TokenType::TOKEN_ENUM)) {
        size_t savedPos = m_currentTokenPos;
        auto decl = parseDeclaration();
        if (decl) {
            applyLocation(decl.get(), stmtStart ? &*stmtStart : nullptr);
            return makeDeclStmt(std::move(decl));
        }
        m_currentTokenPos = savedPos;
    }

    // Local storage classes: `static T x;` / `thread_local T x;`
    // (and `static thread_local`). C gives block-scope thread_local static
    // storage duration, so both forms allocate at module scope.
    if (check(TokenType::TOKEN_STATIC) || check(TokenType::TOKEN_THREAD_LOCAL) ||
        check(TokenType::TOKEN_EXTERN)) {
        // SG-02: a block-scope `extern` may declare a function prototype.
        size_t storageStart = m_currentTokenPos;
        bool isStatic = false;
        bool isTls = false;
        bool isExtern = false;
        while (check(TokenType::TOKEN_STATIC) || check(TokenType::TOKEN_THREAD_LOCAL) ||
               check(TokenType::TOKEN_EXTERN)) {
            if (check(TokenType::TOKEN_STATIC)) {
                advance();
                isStatic = true;
            } else if (check(TokenType::TOKEN_THREAD_LOCAL)) {
                advance();
                isTls = true;
            } else {
                advance();
                isExtern = true;
            }
        }
        if (!isStatic && !isExtern && isTls) isStatic = true;
        if (!(isTypeStart() || check(TokenType::TOKEN_CONST) ||
              check(TokenType::TOKEN_VOLATILE) || check(TokenType::TOKEN_ATOMIC))) {
            errorUnexpected("expected a variable declaration after storage class");
            return std::make_unique<NullStmtAST>();
        }
        Type* type = parseType();
        if (!type || !check(TokenType::TOKEN_IDENTIFIER)) {
            errorUnexpected("expected variable name in storage-class declaration");
            return std::make_unique<NullStmtAST>();
        }
        auto nameTok = advance();
        if (isExtern && check(TokenType::TOKEN_LPAREN)) {
            m_currentTokenPos = storageStart;
            auto decl = parseDeclaration();
            if (decl) {
                applyLocation(decl.get(), stmtStart ? &*stmtStart : nullptr);
                return makeDeclStmt(std::move(decl));
            }
            m_currentTokenPos = storageStart;
        }
        auto varDecl = parseVariableDeclList(type, nameTok->lexeme);
        if (!varDecl) return nullptr;
        std::function<void(DeclAST*)> mark = [&mark, isStatic, isTls, isExtern](DeclAST* d) {
            if (auto* var = dynamic_cast<VarDeclAST*>(d)) {
                var->isStaticLocal = isStatic;
                var->isThreadLocal = isTls;
                var->isExternLocal = isExtern;
            } else if (auto* multi = dynamic_cast<MultiVarDeclAST*>(d)) {
                for (auto& sub : multi->decls) mark(sub.get());
            }
        };
        mark(varDecl.get());
        applyLocation(varDecl.get(), stmtStart ? &*stmtStart : nullptr);
        return makeDeclStmt(std::move(varDecl));
    }

    // local compile_time declaration: `compile_time T name = ...;`
    if (check(TokenType::TOKEN_CONSTEXPR) || isCompileTimeModifierStart()) {
        auto decl = parseDeclaration();
        if (decl) {
            return makeDeclStmt(std::move(decl));
        }
    }

    if (isCompileOnlyStart()) {
        auto decl = parseCompileOnlyDeclaration();
        if (decl) return makeDeclStmt(std::move(decl));
    }

    // PAR-12: 局部 class 定义 `class { ... } c;` / `class C { ... };` ——
    // isTypeStart 不含 TOKEN_CLASS（class 仅作声明起始），走 parseDeclaration
    // 复用顶层 class 分支（含 attachedVars 内联变量）。局部 struct/union 既有
    // 内联路径不经此（isTypeStart 含 struct/union）。
    if (check(TokenType::TOKEN_CLASS)) {
        auto decl = parseDeclaration();
        if (decl) {
            return makeDeclStmt(std::move(decl));
        }
    }

    // PAR-24: legacy alias forms and block-scope using get targeted diagnostics.
    if (isRemovedTypeAliasStart()) {
        rejectRemovedTypeAlias();
        return std::make_unique<NullStmtAST>();
    }
    // local declaration (type keyword or typedef name)
    if (isTypeStart() || check(TokenType::TOKEN_CONST) || check(TokenType::TOKEN_VOLATILE) ||
        check(TokenType::TOKEN_ATOMIC)) {
        size_t savedPos = m_currentTokenPos;
        Type* type = parseType();
        // A function-pointer declarator starts with '(' after the type
        // (e.g. `int (*fp)(int)`); let parseDeclaration handle it.
        if (type && check(TokenType::TOKEN_LPAREN)) {
            m_currentTokenPos = savedPos;
            auto decl = parseDeclaration();
            if (decl) {
                return makeDeclStmt(std::move(decl));
            }
            m_currentTokenPos = savedPos;
        }
        if (type && check(TokenType::TOKEN_IDENTIFIER)) {
            auto nameTok = advance();
            if (check(TokenType::TOKEN_LPAREN)) {
                // This is a function declaration inside a block - restore and parse as declaration
                m_currentTokenPos = savedPos;
                auto decl = parseDeclaration();
                if (decl) {
                    return makeDeclStmt(std::move(decl));
                }
            }
            auto varDecl = parseVariableDeclList(type, nameTok->lexeme);
            if (varDecl) {
                applyLocation(varDecl.get(), stmtStart ? &*stmtStart : nullptr);
                return makeDeclStmt(std::move(varDecl));
            }
        }
        // Not a declaration, restore position
        m_currentTokenPos = savedPos;
    }

    // expression statement
    return parseExprStmt();
}

std::unique_ptr<StmtAST> Parser::parseIfStmt() {
    auto ifToken = match(TokenType::TOKEN_IF);
    if (!ifToken) {
        return nullptr;
    }

    if (check(TokenType::TOKEN_IDENTIFIER) && peek()->lexeme == "compile_time" &&
        m_currentTokenPos + 1 < m_tokens.size() &&
        m_tokens[m_currentTokenPos + 1].type == TokenType::TOKEN_LBRACE) {
        advance(); // compile_time
        auto thenStmt = parseCompoundStmt();
        std::unique_ptr<CompoundStmtAST> elseStmt;
        if (match(TokenType::TOKEN_ELSE)) {
            if (!check(TokenType::TOKEN_LBRACE)) {
                errorUnexpected("expected '{' after 'else' in if compile_time");
            } else {
                elseStmt = parseCompoundStmt();
            }
        }
        auto stmt = std::make_unique<CompileTimeContextIfStmtAST>();
        stmt->thenStmt = std::move(thenStmt);
        stmt->elseStmt = std::move(elseStmt);
        stmt->setLocation(ifToken->filename, ifToken->line, ifToken->column);
        return stmt;
    }

    expect(TokenType::TOKEN_LPAREN, "expected '(' after 'if'");
    auto cond = parseExpr();
    if (!cond) {
        errorUnexpected("expected condition expression");
    }
    expect(TokenType::TOKEN_RPAREN, "expected ')' after condition");

    auto thenStmt = parseStmt();
    if (!thenStmt) {
        errorUnexpected("expected statement after 'if' condition");
    }

    std::unique_ptr<StmtAST> elseStmt;
    if (match(TokenType::TOKEN_ELSE)) {
        elseStmt = parseStmt();
        if (!elseStmt) {
            errorUnexpected("expected statement after 'else'");
        }
    }

    auto stmt = std::make_unique<IfStmtAST>(std::move(cond), std::move(thenStmt), std::move(elseStmt));
    stmt->setLocation(ifToken->filename, ifToken->line, ifToken->column);
    return stmt;
}

std::unique_ptr<StmtAST> Parser::parseWhileStmt() {
    auto whileToken = match(TokenType::TOKEN_WHILE);
    if (!whileToken) {
        return nullptr;
    }

    expect(TokenType::TOKEN_LPAREN, "expected '(' after 'while'");
    auto cond = parseExpr();
    if (!cond) {
        errorUnexpected("expected condition expression");
    }
    expect(TokenType::TOKEN_RPAREN, "expected ')' after condition");

    auto body = parseStmt();
    if (!body) {
        errorUnexpected("expected statement after 'while' condition");
    }

    auto stmt = std::make_unique<WhileStmtAST>(std::move(cond), std::move(body));
    stmt->setLocation(whileToken->filename, whileToken->line, whileToken->column);
    return stmt;
}

std::unique_ptr<StmtAST> Parser::parseDoWhileStmt() {
    auto doToken = match(TokenType::TOKEN_DO);
    if (!doToken) {
        return nullptr;
    }

    auto body = parseStmt();
    if (!body) {
        errorUnexpected("expected statement after 'do'");
    }

    expect(TokenType::TOKEN_WHILE, "expected 'while' after 'do' body");
    expect(TokenType::TOKEN_LPAREN, "expected '(' after 'while'");
    auto cond = parseExpr();
    if (!cond) {
        errorUnexpected("expected condition expression");
    }
    expect(TokenType::TOKEN_RPAREN, "expected ')' after condition");
    expect(TokenType::TOKEN_SEMICOLON, "expected ';' after do-while statement");

    auto stmt = std::make_unique<DoWhileStmtAST>(std::move(cond), std::move(body));
    stmt->setLocation(doToken->filename, doToken->line, doToken->column);
    return stmt;
}

std::unique_ptr<StmtAST> Parser::parseForStmt() {
    auto forToken = match(TokenType::TOKEN_FOR);
    if (!forToken) {
        return nullptr;
    }

    expect(TokenType::TOKEN_LPAREN, "expected '(' after 'for'");

    // init
    std::unique_ptr<StmtAST> init;
    if (peek() && peek()->type == TokenType::TOKEN_SEMICOLON) {
        advance(); // empty init
    } else {
        init = parseStmt(); // could be declaration or expression stmt
    }

    // condition
    std::unique_ptr<ExprAST> cond;
    if (peek() && peek()->type != TokenType::TOKEN_SEMICOLON) {
        cond = parseExpr();
    }
    expect(TokenType::TOKEN_SEMICOLON, "expected ';' after for condition");

    // increment
    std::unique_ptr<ExprAST> inc;
    if (peek() && peek()->type != TokenType::TOKEN_RPAREN) {
        inc = parseExpr();
    }
    expect(TokenType::TOKEN_RPAREN, "expected ')' after for increment");

    auto body = parseStmt();
    if (!body) {
        errorUnexpected("expected statement after 'for' header");
    }

    auto stmt = std::make_unique<ForStmtAST>(std::move(init), std::move(cond), std::move(inc), std::move(body));
    stmt->setLocation(forToken->filename, forToken->line, forToken->column);
    return stmt;
}

std::unique_ptr<StmtAST> Parser::parseSwitchStmt() {
    auto switchToken = match(TokenType::TOKEN_SWITCH);
    if (!switchToken) {
        return nullptr;
    }

    expect(TokenType::TOKEN_LPAREN, "expected '(' after 'switch'");
    auto cond = parseExpr();
    if (!cond) {
        errorUnexpected("expected expression after 'switch ('");
    }
    expect(TokenType::TOKEN_RPAREN, "expected ')' after switch condition");
    if (!expect(TokenType::TOKEN_LBRACE, "expected '{' after switch condition")) {
        return nullptr;
    }

    std::vector<std::unique_ptr<StmtAST>> cases;
    std::vector<std::unique_ptr<ExprAST>> labels;
    std::vector<std::unique_ptr<StmtAST>> body;
    bool haveGroup = false;

    // Statements belong to the most recent label; push the previous group when
    // a new label starts so `cases` and `caseLabels` stay index-aligned.
    auto flushGroup = [&]() {
        if (!haveGroup) return;
        cases.push_back(std::make_unique<CompoundStmtAST>(std::move(body)));
        body.clear();
        haveGroup = false;
    };

    while (!eof() && !check(TokenType::TOKEN_RBRACE)) {
        if (match(TokenType::TOKEN_CASE)) {
            flushGroup();
            auto label = parseExpr();
            if (!label) {
                errorUnexpected("expected constant expression after 'case'");
                break;
            }
            expect(TokenType::TOKEN_COLON, "expected ':' after case label");
            labels.push_back(std::move(label));
            haveGroup = true;
        } else if (match(TokenType::TOKEN_DEFAULT)) {
            flushGroup();
            expect(TokenType::TOKEN_COLON, "expected ':' after 'default'");
            labels.push_back(nullptr);
            haveGroup = true;
        } else {
            auto inner = parseStmt();
            if (!inner) break;
            body.push_back(std::move(inner));
        }
    }
    flushGroup();
    expect(TokenType::TOKEN_RBRACE, "expected '}' to close switch");

    auto stmt = std::make_unique<SwitchStmtAST>(std::move(cond), std::move(cases));
    stmt->caseLabels = std::move(labels);
    stmt->setLocation(switchToken->filename, switchToken->line, switchToken->column);
    return stmt;
}

std::unique_ptr<StmtAST> Parser::parseBreakStmt() {
    auto breakToken = match(TokenType::TOKEN_BREAK);
    if (!breakToken) {
        return nullptr;
    }

    expect(TokenType::TOKEN_SEMICOLON, "expected ';' after 'break'");

    auto stmt = std::make_unique<BreakStmtAST>();
    stmt->setLocation(breakToken->filename, breakToken->line, breakToken->column);
    return stmt;
}

std::unique_ptr<StmtAST> Parser::parseContinueStmt() {
    auto continueToken = match(TokenType::TOKEN_CONTINUE);
    if (!continueToken) {
        return nullptr;
    }

    expect(TokenType::TOKEN_SEMICOLON, "expected ';' after 'continue'");

    auto stmt = std::make_unique<ContinueStmtAST>();
    stmt->setLocation(continueToken->filename, continueToken->line, continueToken->column);
    return stmt;
}

std::unique_ptr<StmtAST> Parser::parseExprStmt() {
    auto startTok = peek();
    auto expr = parseExpr();
    if (!expr) {
        return nullptr;
    }
    expect(TokenType::TOKEN_SEMICOLON, "expected ';' after expression statement");
    auto stmt = std::make_unique<ExprStmtAST>(std::move(expr));
    if (startTok) {
        stmt->setLocation(startTok->filename, startTok->line, startTok->column);
    }
    return stmt;
}

std::unique_ptr<TranslationUnitAST> Parser::parse() {
    std::vector<std::unique_ptr<DeclAST>> decls{};
    
    // 新增：支持模块声明
    std::string moduleName;
    std::vector<std::string> imports;
    std::vector<std::string> exports;
    bool sawModule = false;
    bool sawOtherTopLevel = false;

    while (!eof()) {
        // P1-05 / ANN-01: `[[...]] module ...;` ——模块注解中转至 ModuleDeclAST；
        // 其余声明交由 parseDeclaration 统一解析注解（此处回退）。
        if (isAnnotationStart()) {
            size_t saved = m_currentTokenPos;
            auto anns = parseAnnotations();
            if (check(TokenType::TOKEN_MODULE)) {
                m_pendingModuleAnnotations = std::move(anns);
            } else {
                m_currentTokenPos = saved;
            }
        }

        // MOD-13: `module` must be the first declaration (annotations may
        // precede it). Any other token before it is a violation.
        if (!check(TokenType::TOKEN_MODULE)) sawOtherTopLevel = true;

        // namespace declaration
        if (check(TokenType::TOKEN_NAMESPACE)) {
            auto decl = parseNamespaceDecl();
            if (decl) {
                decls.push_back(std::move(decl));
            } else {
                advance();
            }
            continue;
        }

        // 检查模块声明：`module a.b.c;`（module 只用点分路径）
        if (check(TokenType::TOKEN_MODULE)) {
            auto moduleTok = peek();
            advance(); // 消耗 "module"
            if (sawModule) {
                error("duplicate module declaration", *moduleTok);
            } else if (sawOtherTopLevel) {
                error("module declaration must appear before other declarations", *moduleTok);
            }
            sawModule = true;
            if (!eof() && isModuleNameToken(*peek())) {
                moduleName = advance()->lexeme;
                for (;;) {
                    if (check(TokenType::TOKEN_DOT)) {
                        advance();
                        if (eof() || !isModuleNameToken(*peek())) break;
                        moduleName += "." + advance()->lexeme;
                        continue;
                    }
                    if (check(TokenType::TOKEN_COLON_COLON)) {
                        error("module names use '.'; '::' is reserved for namespaces",
                              *peek());
                        // Skip the malformed qualifier so the following ';' is
                        // still consumed and the parser does not cascade.
                        while (!eof() && !check(TokenType::TOKEN_SEMICOLON)) advance();
                    }
                    break;
                }
            }
            if (moduleName.empty()) {
                error("expected module name after 'module'", *moduleTok);
            }
            match(TokenType::TOKEN_SEMICOLON);
            continue;
        }
        
        // import declaration: `import a.b;` or `import "dir/file.cmm";`
        if (check(TokenType::TOKEN_IMPORT)) {
            advance(); // consume "import"
            std::string importName;
            if (check(TokenType::TOKEN_STRING)) {
                std::string lex = advance()->lexeme;
                if (lex.size() >= 2 && lex.front() == '"' && lex.back() == '"') {
                    importName = lex.substr(1, lex.size() - 2);
                }
            } else if (!eof() && isModuleNameToken(*peek())) {
                importName = advance()->lexeme;
                for (;;) {
                    if (check(TokenType::TOKEN_DOT)) {
                        advance();
                        if (eof() || !isModuleNameToken(*peek())) break;
                        importName += "." + advance()->lexeme;
                        continue;
                    }
                    if (check(TokenType::TOKEN_COLON_COLON)) {
                        error("import specifiers use '.' or a quoted path; "
                              "'::' is reserved for namespaces",
                              *peek());
                        // The whole specifier is invalid; drop it so the loader
                        // does not add a second diagnostic for a partial name.
                        importName.clear();
                        while (!eof() && !check(TokenType::TOKEN_SEMICOLON)) advance();
                    }
                    break;
                }
            }
            if (!importName.empty()) {
                imports.push_back(importName);
            }
            match(TokenType::TOKEN_SEMICOLON);
            continue;
        }
        
        // 检查导出声明：`export`/`public` 是可见性修饰符，后接一个完整声明
        // （P0-04 / MOD-05）。`export namespace A { ... }` 会导出 A 的全部成员。
        if (check(TokenType::TOKEN_EXPORT) || check(TokenType::TOKEN_PUBLIC)) {
            advance(); // 消耗 "export"/"public"
            // `export namespace A { ... }` exports the whole namespace.
            if (check(TokenType::TOKEN_NAMESPACE)) {
                auto nsDecl = parseNamespaceDecl();
                if (nsDecl) {
                    nsDecl->isExported = true;
                    decls.push_back(std::move(nsDecl));
                } else {
                    advance();
                }
                continue;
            }
            size_t declStart = m_currentTokenPos;
            auto decl = parseDeclaration();
            if (decl) {
                decl->isExported = true;
                decls.push_back(std::move(decl));
            } else if (m_currentTokenPos == declStart) {
                advance(); // no progress: skip one token
            }
            continue;
        }

        // Reject C preprocessor directives at the top level (NG-02).
        if (rejectPreprocessorDirective()) {
            continue;
        }

        size_t declStart = m_currentTokenPos;
        auto decl = parseDeclaration();
        if (decl) {
            decls.push_back(std::move(decl));
        } else if (m_currentTokenPos == declStart) {
            advance(); // no progress: skip one token (recover without cascading)
        }
    }
    
    auto tu = std::make_unique<TranslationUnitAST>(std::move(decls));
    tu->imports = imports;
    tu->moduleName = moduleName;

    // 如果有模块声明，创建模块声明节点（imports 已记录在 TU 上）
    if (!moduleName.empty()) {
        auto mod = std::make_unique<ModuleDeclAST>(moduleName, imports, std::move(exports));
        mod->annotations = std::move(m_pendingModuleAnnotations);
        tu->declarations.push_back(std::move(mod));
    }

    // Tag this unit's own declarations with the owning module so the semantic
    // analyzer can apply module visibility (P0-04 / MOD-05/06). Imported
    // modules are tagged separately by the ModuleLoader.
    if (!moduleName.empty()) {
        for (auto& decl : tu->declarations) {
            if (decl && decl->moduleName.empty()) {
                decl->moduleName = moduleName;
            }
        }
    }

    return tu;
}

bool Parser::check(TokenType type) const {
    if (eof()) return false;
    return m_tokens[m_currentTokenPos].type == type;
}

bool Parser::isTypeStart() const {
    if (eof()) return false;
    switch (peek()->type) {
        case TokenType::TOKEN_CHAR_KW:
        case TokenType::TOKEN_VOID:
        case TokenType::TOKEN_BOOL:
        case TokenType::TOKEN_AUTO:
        case TokenType::TOKEN_STRUCT:
        case TokenType::TOKEN_UNION:
        case TokenType::TOKEN_ENUM:
        case TokenType::TOKEN_TYPEDEF:
        case TokenType::TOKEN_INT8:
        case TokenType::TOKEN_INT16:
        case TokenType::TOKEN_INT32:
        case TokenType::TOKEN_INT64:
        case TokenType::TOKEN_INT128:
        case TokenType::TOKEN_UINT8:
        case TokenType::TOKEN_UINT16:
        case TokenType::TOKEN_UINT32:
        case TokenType::TOKEN_UINT64:
        case TokenType::TOKEN_UINT128:
        case TokenType::TOKEN_CHAR8:
        case TokenType::TOKEN_CHAR16:
        case TokenType::TOKEN_CHAR32:
        case TokenType::TOKEN_ISIZE:
        case TokenType::TOKEN_USIZE:
        case TokenType::TOKEN_FLOAT32:
        case TokenType::TOKEN_FLOAT64:
        case TokenType::TOKEN_FLOAT16:
        case TokenType::TOKEN_FLOAT128:
            return true;
        case TokenType::TOKEN_IDENTIFIER: {
            // A possibly `::`-qualified typedef/class/struct name (lookahead so
            // the cursor is not disturbed).
            size_t i = m_currentTokenPos;
            if (i >= m_tokens.size()) return false;
            std::string name = m_tokens[i].lexeme;
            // P1-03 / GEN-01: 模板类型参数名（模板体内）是类型起始——
            // TypeVar 不入类型表，须先于 lookupNamedType 判断。
            if (!m_templateScopes.empty() &&
                m_templateScopes.back().typeParams.count(name)) {
                return true;
            }
            while (i + 2 < m_tokens.size() &&
                   m_tokens[i + 1].type == TokenType::TOKEN_COLON_COLON &&
                   m_tokens[i + 2].type == TokenType::TOKEN_IDENTIFIER) {
                name += "::" + m_tokens[i + 2].lexeme;
                i += 2;
            }
            // P1-06: builtin polymorphic type names are lowercase —
            // `optional<T>` / `result<T, E>` (DEC-18 hard-rename precedent;
            // the old capitalized spellings are ordinary unknown identifiers).
            if ((name == "optional" || name == "result") &&
                i + 1 < m_tokens.size() &&
                m_tokens[i + 1].type == TokenType::TOKEN_LT) {
                return true;
            }
            // P1-06 (FMT-01/02) + I4/D3: builtin monomorphic types
            // `string_view` (原 `str` 硬改名) / `string`.
            if (name == "string" || name == "string_view") {
                return true;
            }
            // CT-07: reflection result types.
            if (name == "FieldInfo" || name == "Attribute") {
                return true;
            }
            return lookupNamedType(name) != nullptr;
        }
        default:
            return false;
    }
}

std::string Parser::parseQualifiedTypeName() {
    std::string name;
    if (!check(TokenType::TOKEN_IDENTIFIER)) {
        return name;
    }
    name = advance()->lexeme;
    while (check(TokenType::TOKEN_COLON_COLON)) {
        size_t saved = m_currentTokenPos;
        advance(); // consume '::'
        if (!check(TokenType::TOKEN_IDENTIFIER)) {
            m_currentTokenPos = saved;
            break;
        }
        name += "::" + advance()->lexeme;
    }
    return name;
}

std::string Parser::qualifyTypeDeclName(const std::string& name) const {
    if (name.empty()) return name;
    // PAR-12: 合成名（`__anon_<N>`）进程内全局唯一，不再加 namespace 前缀
    // ——否则 `struct Outer { struct { ... }; };` 里前缀化后
    // `S___anon_0` 使 isAnonymousAggregate 的 `__anon_` 前缀判定失配。
    if (name.rfind("__anon_", 0) == 0) return name;
    if (name.find("::") != std::string::npos || name.find('.') != std::string::npos) {
        return mangleQualifiedTypeName(name);
    }
    if (!m_typeNamespacePrefix.empty()) {
        return m_typeNamespacePrefix + name;
    }
    return name;
}

Type* Parser::lookupNamedType(const std::string& name) const {
    if (name.empty()) return nullptr;
    auto& tc = TypeContext::instance();
    auto tryKey = [&tc](const std::string& key) -> Type* {
        if (Type* t = tc.getTypedef(key)) return t;
        if (ClassType* c = tc.getClass(key)) return c;
        if (StructType* s = tc.getStruct(key)) return s;
        if (UnionType* u = tc.getUnion(key)) return u;
        if (EnumType* e = tc.getEnum(key)) return e;
        return nullptr;
    };
    if (name.find("::") != std::string::npos || name.find('.') != std::string::npos) {
        return tryKey(mangleQualifiedTypeName(name));
    }
    // PAR-24: block-scoped aliases shadow unqualified outer names.
    if (Type* t = tc.getLocalTypedef(name)) return t;
    // Unqualified: prefer the enclosing namespace, then the global name.
    if (!m_typeNamespacePrefix.empty()) {
        if (Type* t = tryKey(m_typeNamespacePrefix + name)) return t;
    }
    return tryKey(name);
}

Type* Parser::parseBaseType() {
    auto tok = peek();
    if (!tok) return nullptr;

    switch (tok->type) {
        case TokenType::TOKEN_AUTO: {
            advance();
            return new Type(TypeKind::Auto);
        }
        case TokenType::TOKEN_CHAR_KW: {
            advance();
            return TypeContext::instance().getChar();
        }
        case TokenType::TOKEN_VOID: {
            advance();
            return TypeContext::instance().getVoid();
        }
        case TokenType::TOKEN_BOOL: {
            advance();
            return TypeContext::instance().getBool();
        }
        case TokenType::TOKEN_INT8: {
            advance();
            return TypeContext::instance().getInt8();
        }
        case TokenType::TOKEN_INT16: {
            advance();
            return TypeContext::instance().getInt16();
        }
        case TokenType::TOKEN_INT32: {
            advance();
            return TypeContext::instance().getInt32();
        }
        case TokenType::TOKEN_INT64: {
            advance();
            return TypeContext::instance().getInt64();
        }
        case TokenType::TOKEN_INT128: {
            advance();
            return TypeContext::instance().getInt128();
        }
        case TokenType::TOKEN_UINT8: {
            advance();
            return TypeContext::instance().getUInt8();
        }
        case TokenType::TOKEN_UINT16: {
            advance();
            return TypeContext::instance().getUInt16();
        }
        case TokenType::TOKEN_UINT32: {
            advance();
            return TypeContext::instance().getUInt32();
        }
        // TYP-05: char8/char16/char32 are aliases of the unsigned fixed-width
        // integers (design decision — no nominal char types).
        case TokenType::TOKEN_CHAR8: {
            advance();
            return TypeContext::instance().getUInt8();
        }
        case TokenType::TOKEN_CHAR16: {
            advance();
            return TypeContext::instance().getUInt16();
        }
        case TokenType::TOKEN_CHAR32: {
            advance();
            return TypeContext::instance().getUInt32();
        }
        case TokenType::TOKEN_UINT64: {
            advance();
            return TypeContext::instance().getUInt64();
        }
        case TokenType::TOKEN_UINT128: {
            advance();
            return TypeContext::instance().getUInt128();
        }
        case TokenType::TOKEN_ISIZE: {
            advance();
            return TypeContext::instance().getISize();
        }
        case TokenType::TOKEN_USIZE: {
            advance();
            return TypeContext::instance().getUSize();
        }
        case TokenType::TOKEN_FLOAT32: {
            advance();
            return TypeContext::instance().getFloat32();
        }
        case TokenType::TOKEN_FLOAT64: {
            advance();
            return TypeContext::instance().getFloat64();
        }
        case TokenType::TOKEN_FLOAT16: {
            advance();
            return TypeContext::instance().getFloat16();
        }
        case TokenType::TOKEN_FLOAT128: {
            advance();
            return TypeContext::instance().getFloat128();
        }
        case TokenType::TOKEN_STRUCT: {
            advance(); // consume 'struct'
            std::string name = parseQualifiedTypeName();
            // PAR-12: 匿名定义 `struct { ... }` 获得唯一合成名（C 语义：每次
            // 匿名定义 = 独立类型）。
            const bool isAnonymous = name.empty();
            if (isAnonymous && check(TokenType::TOKEN_LBRACE)) {
                name = TypeContext::instance().nextAnonId();
            }
            std::string key = qualifyTypeDeclName(name);

            // Check if this is a definition or just a reference
            if (check(TokenType::TOKEN_LBRACE)) {
                advance(); // consume '{'
                // MOD-03: reuse an earlier forward placeholder so aliases and
                // pointers parsed before the definition see the fields.
                StructType* structType = nullptr;
                if (!isAnonymous) structType = TypeContext::instance().getStruct(key);
                if (!structType) {
                    structType = new StructType(key);
                } else {
                    structType->fields.clear();
                }
                while (!eof() && !check(TokenType::TOKEN_RBRACE)) {
                    Type* fieldType = parseType();
                    if (!fieldType) break;
                    // P1-07 / PAR-10: unnamed bit-field `T : N;`.
                    if (check(TokenType::TOKEN_COLON)) {
                        advance();
                        int bw = 0;
                        if (!parseBitFieldWidth(bw)) break;
                        structType->addField("", fieldType, bw);
                        match(TokenType::TOKEN_SEMICOLON);
                        continue;
                    }
                    if (!check(TokenType::TOKEN_IDENTIFIER) &&
                        !check(TokenType::TOKEN_STAR) &&
                        !check(TokenType::TOKEN_LPAREN)) {
                        break;
                    }
                    Type* memberBase = stripDeclaratorType(fieldType);
                    bool firstField = true;
                    while (true) {
                        MemberFieldDecl fd;
                        if (!parseMemberFieldDecl(firstField ? fieldType : memberBase, fd))
                            break;
                        structType->addField(fd.name, fd.type, fd.bitWidth);
                        if (fd.flexibleArray && !structType->fields.empty())
                            structType->fields.back().flexibleArray = true;
                        firstField = false;
                        if (!match(TokenType::TOKEN_COMMA)) {
                            match(TokenType::TOKEN_SEMICOLON);
                            break;
                        }
                    }
                }
                match(TokenType::TOKEN_RBRACE);
                match(TokenType::TOKEN_SEMICOLON);
                structType->parsedDefinition = true;
                TypeContext::instance().addStruct(key, structType);
                return structType;
            }

            // Reference to an existing record (qualified or, inside a
            // namespace, the global name). PAR-05: a class-like struct is
            // registered as ClassType, so `struct S` must consult both maps;
            // ClassType wins over a stale StructType forward placeholder.
            if (!name.empty()) {
                if (auto* existing = TypeContext::instance().getClass(key)) return existing;
                if (auto* existing = TypeContext::instance().getStruct(key)) return existing;
                if (key != name) {
                    if (auto* existing = TypeContext::instance().getClass(name)) return existing;
                    if (auto* existing = TypeContext::instance().getStruct(name)) return existing;
                }
            }

            // Forward reference - create placeholder under the qualified key
            auto* structType = new StructType(key);
            TypeContext::instance().addStruct(key, structType);
            return structType;
        }
        case TokenType::TOKEN_UNION: {
            advance(); // consume 'union'
            std::string name = parseQualifiedTypeName();
            // PAR-12: 匿名 union 定义获得唯一合成名。
            const bool isAnonymous = name.empty();
            if (isAnonymous && check(TokenType::TOKEN_LBRACE)) {
                name = TypeContext::instance().nextAnonId();
            }
            std::string key = qualifyTypeDeclName(name);

            if (check(TokenType::TOKEN_LBRACE)) {
                advance(); // consume '{'
                // MOD-03: mirror the struct placeholder reuse.
                UnionType* unionType = nullptr;
                if (!isAnonymous) unionType = TypeContext::instance().getUnion(key);
                if (!unionType) {
                    unionType = new UnionType(key);
                } else {
                    unionType->members.clear();
                }
                while (!eof() && !check(TokenType::TOKEN_RBRACE)) {
                    Type* memberType = parseType();
                    if (!memberType) break;
                    // P1-07 / PAR-10: unnamed bit-field `T : N;`.
                    if (check(TokenType::TOKEN_COLON)) {
                        advance();
                        int bw = 0;
                        if (!parseBitFieldWidth(bw)) break;
                        unionType->addMember("", memberType, bw);
                        match(TokenType::TOKEN_SEMICOLON);
                        continue;
                    }
                    if (!check(TokenType::TOKEN_IDENTIFIER) &&
                        !check(TokenType::TOKEN_STAR) &&
                        !check(TokenType::TOKEN_LPAREN)) {
                        break;
                    }
                    Type* memberBase = stripDeclaratorType(memberType);
                    bool firstField = true;
                    while (true) {
                        MemberFieldDecl fd;
                        if (!parseMemberFieldDecl(firstField ? memberType : memberBase, fd))
                            break;
                        unionType->addMember(fd.name, fd.type, fd.bitWidth);
                        if (fd.flexibleArray && !unionType->members.empty())
                            unionType->members.back().flexibleArray = true;
                        firstField = false;
                        if (!match(TokenType::TOKEN_COMMA)) {
                            match(TokenType::TOKEN_SEMICOLON);
                            break;
                        }
                    }
                }
                match(TokenType::TOKEN_RBRACE);
                match(TokenType::TOKEN_SEMICOLON);
                unionType->parsedDefinition = true;
                TypeContext::instance().addUnion(key, unionType);
                return unionType;
            }

            if (!name.empty()) {
                if (auto* existing = TypeContext::instance().getUnion(key)) return existing;
                if (key != name) {
                    if (auto* existing = TypeContext::instance().getUnion(name)) return existing;
                }
            }

            auto* unionType = new UnionType(key);
            TypeContext::instance().addUnion(key, unionType);
            return unionType;
        }
        case TokenType::TOKEN_ENUM: {
            advance(); // consume 'enum'
            std::string name = parseQualifiedTypeName();
            std::string key = qualifyTypeDeclName(name);

            // Optional explicit underlying type (TYP-09/TYP-25), e.g.
            // `typedef enum : uint16 { ... } Name;`.
            Type* underlyingType = nullptr;
            if (check(TokenType::TOKEN_COLON)) {
                advance(); // consume ':'
                underlyingType = parseType();
                if (!underlyingType) {
                    errorUnexpected("expected underlying type after ':'");
                    return nullptr;
                }
            }

            if (check(TokenType::TOKEN_LBRACE)) {
                advance(); // consume '{'
                auto* enumType = new EnumType(key);
                enumType->underlyingType = underlyingType;
                int currentVal = 0;
                while (!eof() && !check(TokenType::TOKEN_RBRACE)) {
                    if (!check(TokenType::TOKEN_IDENTIFIER)) break;
                    std::string valueName = advance()->lexeme;
                    int val = currentVal;
                    if (check(TokenType::TOKEN_ASSIGN)) {
                        advance();
                        auto expr = parseExpr(2); // ',' separates enumerators
                        long long evaluated = 0;
                        if (evalIntConstExpr(expr.get(), evaluated)) {
                            val = static_cast<int>(evaluated);
                        }
                    }
                    enumType->addValue(valueName, val);
                    currentVal = val + 1;
                    if (!check(TokenType::TOKEN_COMMA)) break;
                    advance(); // consume ','
                }
                match(TokenType::TOKEN_RBRACE);
                TypeContext::instance().addEnum(key, enumType);
                return enumType;
            }

            if (!name.empty()) {
                if (auto* existing = TypeContext::instance().getEnum(key)) return existing;
                if (key != name) {
                    if (auto* existing = TypeContext::instance().getEnum(name)) return existing;
                }
            }

            auto* enumType = new EnumType(key);
            enumType->underlyingType = underlyingType;
            TypeContext::instance().addEnum(key, enumType);
            return enumType;
        }
        case TokenType::TOKEN_IDENTIFIER: {
            // A (possibly namespace-qualified) named type: typedef/class/struct.
            // Look ahead without consuming so a non-type identifier leaves the
            // cursor untouched for callers that probe.
            size_t saved = m_currentTokenPos;
            std::string name = parseQualifiedTypeName();
            // P1-03 / GEN-01: 模板体内的类型参数名 → TypeVarType 占位。
            if (!m_templateScopes.empty() &&
                m_templateScopes.back().typeParams.count(name)) {
                return TypeContext::instance().getTypeVar(name);
            }
            // P1-06 (FMT-01/02) + I4/D3: builtin monomorphic types
            // `string_view` (原 `str` 硬改名) / `string` — identifier
            // special-cases (same mechanism as optional/result), not keywords.
            if (name == "string_view") {
                return TypeContext::instance().getStrType();
            }
            if (name == "string") {
                return TypeContext::instance().getStringType();
            }
            // CT-07: reflection result types (identifier special-cases).
            if (name == "FieldInfo") {
                return TypeContext::instance().getFieldInfoType();
            }
            if (name == "Attribute") {
                return TypeContext::instance().getAttributeType();
            }
            // P1-02 (TYP-13/14) / P1-06: builtin polymorphic types
            // `optional<T>` and `result<T,E>` — special-cased here (type
            // positions only) since the general template system (GEN) does
            // not exist yet; mirrors the template-instance grammar. `T?`
            // remains the optional sugar.
            if ((name == "optional" || name == "result") && check(TokenType::TOKEN_LT)) {
                advance(); // consume '<'
                auto closeBracket = [this]() -> bool {
                    if (check(TokenType::TOKEN_GT)) {
                        advance();
                        return true;
                    }
                    if (check(TokenType::TOKEN_RSHIFT)) {
                        // Nested args (`Optional<Optional<int32>>`): split the
                        // lexed '>>' into two '>' by rewriting the token in
                        // place; the outer close then consumes the remainder.
                        m_tokens[m_currentTokenPos].type = TokenType::TOKEN_GT;
                        m_tokens[m_currentTokenPos].lexeme = ">";
                        return true;
                    }
                    return false;
                };
                if (name == "optional") {
                    Type* elem = parseType();
                    if (!elem || !closeBracket()) {
                        errorUnexpected("expected type argument list for 'optional<T>'");
                        return nullptr;
                    }
                    return TypeContext::instance().getOptionalType(elem);
                }
                Type* successType = parseType();
                if (!successType || !check(TokenType::TOKEN_COMMA)) {
                    errorUnexpected("expected ',' in 'result<T, E>'");
                    return nullptr;
                }
                advance(); // consume ','
                Type* errorType = parseType();
                if (!errorType || !closeBracket()) {
                    errorUnexpected("expected type argument list for 'result<T, E>'");
                    return nullptr;
                }
                return TypeContext::instance().getResultType(successType, errorType);
            }
            // P1-03 / GEN-03: 类型位置的模板使用 `Name<args>`——产生
            // TypeInstanceType 占位，sema 在使用点实例化并解析为具体类型。
            // 实参可为类型或整型字面量；嵌套 `>>` 按深度拆分（同 Optional）。
            if (check(TokenType::TOKEN_LT)) {
                advance(); // consume '<'
                // 评审 C2：closeBracket 原地拆分 RSHIFT——失败路径（调用方
                // 可能回退光标重解析）必须恢复。
                std::vector<std::pair<size_t, TokenType>> rshiftMutations;
                auto closeBracket = [this, &rshiftMutations]() -> bool {
                    if (check(TokenType::TOKEN_GT)) {
                        advance();
                        return true;
                    }
                    if (check(TokenType::TOKEN_RSHIFT)) {
                        rshiftMutations.emplace_back(m_currentTokenPos, TokenType::TOKEN_RSHIFT);
                        m_tokens[m_currentTokenPos].type = TokenType::TOKEN_GT;
                        m_tokens[m_currentTokenPos].lexeme = ">";
                        return true;
                    }
                    return false;
                };
                auto restoreRshift = [this, &rshiftMutations]() {
                    for (auto& [pos, orig] : rshiftMutations) {
                        m_tokens[pos].type = orig;
                        m_tokens[pos].lexeme = ">>";
                    }
                };
                auto* inst = new TypeInstanceType(name);
                while (true) {
                    Type* argType = nullptr;
                    long long argValue = 0;
                    std::string argName;
                    std::shared_ptr<ExprAST> argExpr;
                    if (!parseTemplateArgument(argType, argValue, argName, argExpr)) {
                        errorUnexpected("expected type or constant template argument");
                        restoreRshift();
                        return nullptr;
                    }
                    if (argType) {
                        inst->typeArgs.push_back(argType);
                    } else {
                        inst->valueArgs.push_back(argValue);
                        inst->valueArgNames.push_back(std::move(argName));
                        inst->valueArgExprs.push_back(std::move(argExpr));
                    }
                    if (match(TokenType::TOKEN_COMMA)) continue;
                    break;
                }
                if (!closeBracket()) {
                    errorUnexpected("expected '>' to close template argument list");
                    restoreRshift();
                    return nullptr;
                }
                return inst;
            }
            if (Type* named = lookupNamedType(name)) {
                return named;
            }
            m_currentTokenPos = saved;
            return nullptr;
        }
        default:
            return nullptr;
    }
}

Type* Parser::parseType() {
    bool isConst = false;
    bool isVolatile = false;
    bool isAtomic = false;

    // Handle qualifiers before base type
    while (!eof()) {
        if (check(TokenType::TOKEN_CONST)) {
            advance();
            isConst = true;
        } else if (check(TokenType::TOKEN_VOLATILE)) {
            advance();
            isVolatile = true;
        } else if (check(TokenType::TOKEN_ATOMIC)) {
            advance();
            isAtomic = true;
        } else {
            break;
        }
    }

    Type* baseType = parseBaseType();
    if (!baseType) return nullptr;

    // C allows declaration specifiers in either order: `const int32` and
    // `int32 const` mean the same type.
    while (!eof()) {
        if (check(TokenType::TOKEN_CONST)) {
            advance();
            isConst = true;
        } else if (check(TokenType::TOKEN_VOLATILE)) {
            advance();
            isVolatile = true;
        } else if (check(TokenType::TOKEN_ATOMIC)) {
            advance();
            isAtomic = true;
        } else {
            break;
        }
    }

    // Qualifiers must not mutate shared scalar singletons (`const char*` was
    // poisoning the global char type, making every later char* const).
    if (isConst || isVolatile) {
        switch (baseType->kind) {
            case TypeKind::Void:
            case TypeKind::Char:
            case TypeKind::Bool:
            case TypeKind::Int8: case TypeKind::Int16:
            case TypeKind::Int32: case TypeKind::Int64:
            case TypeKind::Int128:
            case TypeKind::UInt8: case TypeKind::UInt16:
            case TypeKind::UInt32: case TypeKind::UInt64:
            case TypeKind::UInt128:
            case TypeKind::ISize: case TypeKind::USize:
            case TypeKind::Float16: case TypeKind::Float32:
            case TypeKind::Float64: case TypeKind::Float128: {
                auto* copy = new Type(baseType->kind, baseType->base);
                copy->isConst = baseType->isConst;
                copy->isVolatile = baseType->isVolatile;
                copy->isAtomic = baseType->isAtomic;
                copy->isRestrict = baseType->isRestrict;
                copy->ctDeadBranch = baseType->ctDeadBranch;
                copy->deprecated = baseType->deprecated;
                copy->deprecatedMsg = baseType->deprecatedMsg;
                baseType = copy;
                break;
            }
            default:
                break;
        }
    }
    baseType->isConst = isConst;
    baseType->isVolatile = isVolatile;
    // CON-01: `atomic T` must NOT mutate the shared scalar singletons
    // (getInt32() etc. are process-wide) — clone the type instead. A plain
    // `new Type(kind, base)` clone only preserves scalar kinds; aggregating
    // subclasses (Struct/Class/Union fields, Enum values) would be corrupted
    // and later crash LayoutBuilder. So: strip typedefs to the innermost type;
    // clone only scalar kinds; reject atomic-on-aggregate here (C1/C2 fix).
    if (isAtomic) {
        Type* inner = baseType;
        while (inner && inner->kind == TypeKind::Typedef)
            inner = static_cast<TypedefType*>(inner)->aliasedType;
        if (!inner || inner->kind == TypeKind::Struct || inner->kind == TypeKind::Class ||
            inner->kind == TypeKind::Union || inner->kind == TypeKind::Enum) {
            error("atomic requires an integer scalar type", *peek());
            return nullptr;
        }
        if (inner->kind == TypeKind::TypeVar) {
            // `atomic T` inside a template body: a plain Type(kind, base)
            // clone would lose TypeVarType::name and later crash template
            // instantiation. Preserve the placeholder identity and let the
            // instantiator apply the atomic qualifier to the resolved type.
            auto* tv = static_cast<TypeVarType*>(inner);
            auto* copy = new TypeVarType(tv->name);
            copy->isConst = isConst;
            copy->isVolatile = isVolatile;
            copy->isAtomic = true;
            copy->ctDeadBranch = inner->ctDeadBranch;
            copy->deprecated = inner->deprecated;
            copy->deprecatedMsg = inner->deprecatedMsg;
            baseType = copy;
        } else {
            auto* copy = new Type(inner->kind, inner->base);
            copy->isConst = isConst;
            copy->isVolatile = isVolatile;
            copy->isAtomic = true;
            copy->ctDeadBranch = inner->ctDeadBranch;
            copy->deprecated = inner->deprecated;
            copy->deprecatedMsg = inner->deprecatedMsg;
            baseType = copy;
        }
    }

    // Check for pointer types with qualifiers
    while (check(TokenType::TOKEN_STAR)) {
        advance();
        auto* ptrType = new Type(TypeKind::Pointer, baseType);

        // Check for qualifiers after pointer star (but before next star or identifier)
        while (!eof()) {
            if (check(TokenType::TOKEN_CONST)) {
                advance();
                ptrType->isConst = true;
            } else if (check(TokenType::TOKEN_VOLATILE)) {
                advance();
                ptrType->isVolatile = true;
            } else if (check(TokenType::TOKEN_RESTRICT)) {
                advance();
                ptrType->isRestrict = true;
            } else {
                break;
            }
        }

        baseType = ptrType;
    }

    // 新增：支持切片类型 T[]（可叠加：T[][] = slice of slice，TYP-12）。
    // 非 `[]` 后缀（如声明符侧的 `T[3]`）不在此消耗——固定数组维度只在
    // 复合字面量的 type-name 位置按需解析（见 parseUnaryImpl，PAR-08）。
    while (check(TokenType::TOKEN_LBRACKET)) {
        size_t save = m_currentTokenPos;
        advance(); // 消耗 '['
        if (check(TokenType::TOKEN_RBRACKET)) {
            advance(); // 消耗 ']'
            baseType = TypeContext::instance().getSliceType(baseType);
        } else {
            m_currentTokenPos = save; // restore: not a slice suffix
            break;
        }
    }

    // 新增：支持可选类型 T?
    if (check(TokenType::TOKEN_QUESTION)) {
        advance(); // 消耗 '?'
        return TypeContext::instance().getOptionalType(baseType);
    }

    return baseType;
}

// PAR-24: `using Name = <type-id>;` 的右侧是完整 type-id。
// parseType() 已消费基础类型/指针/切片 `[]`/optional `?`；这里补上只有
// “无声明符名的 type-id”才需要的两种后缀：固定数组维度与抽象函数指针
// `Ret (*)(params)`。
Type* Parser::parseTypeId() {
    Type* type = parseType();
    if (!type) return nullptr;

    // 固定数组后缀（可多维）：`T[2][3]`，从右到左包装。
    if (check(TokenType::TOKEN_LBRACKET)) {
        std::vector<int> dims;
        while (check(TokenType::TOKEN_LBRACKET)) {
            advance(); // '['
            int size = 0;
            bool hasSize = false;
            if (!parseArrayDimension(size, hasSize)) return nullptr;
            if (!hasSize) {
                errorUnexpected("array size must be an integer constant");
                return nullptr;
            }
            if (!expect(TokenType::TOKEN_RBRACKET, "expected ']' after array size"))
                return nullptr;
            dims.push_back(size);
        }
        Type* elem = type;
        for (int i = static_cast<int>(dims.size()) - 1; i >= 1; --i)
            elem = new ArrayType(elem, dims[i]);
        type = new ArrayType(elem, dims[0]);
    }

    // 抽象函数指针/函数类型：`Ret (*)(params)` 或 `Ret(params)`。
    {
        std::string ignored;
        bool isFunction = false;
        if (Type* fp = parseFunctionPointerDeclarator(
                type, ignored, /*requireName=*/false, &isFunction))
            type = fp;
        else if (Type* fn = parseAbstractFunctionSuffix(type))
            type = fn;
    }

    return type;
}

// PAR-24: shared parser for `using IDENT = type-id ;` in block and aggregate
// member scopes. `member=true` registers the flat qualified key and persists;
// `member=false` writes the block-local stack and keeps the parse-time wrapper.
std::unique_ptr<UsingDeclAST> Parser::parseUsingAliasDecl(bool member) {
    advance(); // consume "using"
    std::string bare = advance()->lexeme;
    advance(); // consume '='
    Type* aliased = parseTypeId();
    if (!aliased) {
        skipPastSemicolon();
        return nullptr;
    }
    match(TokenType::TOKEN_SEMICOLON);

    auto u = member
        ? std::make_unique<UsingDeclAST>(qualifyTypeDeclName(bare), aliased)
        : std::make_unique<UsingDeclAST>(bare, aliased);
    u->bareName = bare;
    u->isBlockScope = !member;
    if (member) {
        TypeContext::instance().addTypedef(u->name, aliased);
    } else {
        u->aliasWrapper = TypeContext::instance().addLocalTypedef(bare, aliased);
    }
    return u;
}

// P1-05 / ANN-01: `[[` 判定（连续两个 LBRACKET；注解名非关键字）。
bool Parser::isAnnotationStart() const {
    if (eof() || peek()->type != TokenType::TOKEN_LBRACKET) return false;
    if (m_currentTokenPos + 1 >= m_tokens.size()) return false;
    return m_tokens[m_currentTokenPos + 1].type == TokenType::TOKEN_LBRACKET;
}

// P1-05 / ANN-01: 注解序列解析——`[[name]]` / `[[name(arg, ...)]]`，可连写。
// 实参按文法 §7：string→String、type（isTypeStart）→Type、identifier→Ident、
// 其余 const-expr→Expr（parseExpr(2)：',' 分隔惯例）。
std::vector<Annotation> Parser::parseAnnotations() {
    std::vector<Annotation> result;
    while (isAnnotationStart()) {
        Token open = *peek();
        advance(); // [
        advance(); // [
        // P1-05 Ruling: `inline` 已是关键字（TOKEN_INLINE），注解名额外接受
        // 它；其余注解名均为 identifier。cost: 若未来新增关键字形式注解名需扩此集合。
        if (!check(TokenType::TOKEN_IDENTIFIER) && !check(TokenType::TOKEN_INLINE)) {
            errorUnexpected("expected annotation name after '[['");
            return result;
        }
        Annotation ann;
        ann.name = advance()->lexeme;
        ann.line = open.line;
        ann.column = open.column;
        if (match(TokenType::TOKEN_LPAREN)) {
            while (!check(TokenType::TOKEN_RPAREN) && !eof()) {
                AnnotationArg arg;
                if (peek()) {
                    arg.line = peek()->line;
                    arg.column = peek()->column;
                }
                if (check(TokenType::TOKEN_STRING)) {
                    std::string lex = advance()->lexeme;
                    if (lex.size() >= 2 && lex.front() == '"' && lex.back() == '"') {
                        lex = lex.substr(1, lex.size() - 2);
                    }
                    arg.kind = AnnotationArg::Kind::String;
                    arg.text = lex;
                } else if (isTypeStart()) {
                    arg.kind = AnnotationArg::Kind::Type;
                    arg.type = parseType();
                    if (!arg.type) return result;
                } else if (check(TokenType::TOKEN_IDENTIFIER)) {
                    arg.kind = AnnotationArg::Kind::Ident;
                    arg.text = advance()->lexeme;
                } else {
                    auto expr = parseExpr(2);
                    if (!expr) return result;
                    arg.kind = AnnotationArg::Kind::Expr;
                    arg.expr = std::shared_ptr<ExprAST>(expr.release());
                }
                ann.args.push_back(std::move(arg));
                if (!match(TokenType::TOKEN_COMMA)) break;
            }
            if (!expect(TokenType::TOKEN_RPAREN, "expected ')' after annotation arguments")) {
                return result;
            }
        }
        if (!expect(TokenType::TOKEN_RBRACKET, "expected ']]' after annotation") ||
            !expect(TokenType::TOKEN_RBRACKET, "expected ']]' after annotation")) {
            return result;
        }
        result.push_back(std::move(ann));
    }
    return result;
}

std::unique_ptr<DeclAST> Parser::parseDeclaration() {
    auto startTok = peek();
    // P1-05 / ANN-01: 声明头注解（顶层/namespace 成员/局部声明统一入口）。
    auto anns = parseAnnotations();
    auto decl = parseDeclarationImpl();
    if (decl && !anns.empty()) {
        decl->annotations = std::move(anns);
    }
    if (decl) {
        applyLocation(decl.get(), startTok ? &*startTok : nullptr);
    }
    return decl;
}

// PAR-24: `typedef` / `type` were removed. Recognize their declaration shape
// in every scope so the diagnostic is the same instead of a cascade (block) or
// a silent ignore (class body).
bool Parser::isRemovedTypeAliasStart() const {
    if (check(TokenType::TOKEN_TYPEDEF)) return true;
    if (!check(TokenType::TOKEN_IDENTIFIER) || !peek() || peek()->lexeme != "type")
        return false;
    return m_currentTokenPos + 2 < m_tokens.size() &&
           m_tokens[m_currentTokenPos + 1].type == TokenType::TOKEN_IDENTIFIER &&
           m_tokens[m_currentTokenPos + 2].type == TokenType::TOKEN_ASSIGN;
}

bool Parser::isLocalUsingStart() const {
    if (!check(TokenType::TOKEN_IDENTIFIER) || !peek() || peek()->lexeme != "using")
        return false;
    return m_currentTokenPos + 2 < m_tokens.size() &&
           m_tokens[m_currentTokenPos + 1].type == TokenType::TOKEN_IDENTIFIER &&
           m_tokens[m_currentTokenPos + 2].type == TokenType::TOKEN_ASSIGN;
}

void Parser::skipPastSemicolon() {
    int depth = 0;
    while (!eof()) {
        if (check(TokenType::TOKEN_LBRACE) || check(TokenType::TOKEN_LPAREN) ||
            check(TokenType::TOKEN_LBRACKET)) {
            ++depth;
        } else if (check(TokenType::TOKEN_RBRACE)) {
            if (depth == 0) return; // don't eat the enclosing body's '}'
            --depth;
        } else if (check(TokenType::TOKEN_RPAREN) || check(TokenType::TOKEN_RBRACKET)) {
            if (depth > 0) --depth;
        } else if (check(TokenType::TOKEN_SEMICOLON) && depth == 0) {
            advance();
            return;
        }
        advance();
    }
}

void Parser::rejectRemovedTypeAlias() {
    Token tok = *peek();
    std::string kw = tok.type == TokenType::TOKEN_TYPEDEF ? "typedef" : "type";
    error("'" + kw + "' has been removed; use 'using Name = Type;' instead", tok);
    skipPastSemicolon();
}

std::unique_ptr<DeclAST> Parser::parseDeclarationImpl() {
    // C linkage: `extern` declarations keep their plain, unmangled symbol name.
    if (check(TokenType::TOKEN_EXTERN)) {
        advance();
        auto decl = parseDeclaration();
        if (auto* fn = dynamic_cast<FunctionDeclAST*>(decl.get())) {
            fn->isExtern = true;
            // F2: an extern declared inside a namespace body (`std::sqrt`) is
            // reachable via the `ns::f` → bare-`f` fallback; a global-scope
            // extern (prelude `malloc`/`strlen`) is NOT — namespace isolation
            // preserved. `m_typeNamespacePrefix` is non-empty in namespace
            // (and class) bodies.
            if (m_typeNamespacePrefix.empty()) {
                markCName(fn->name);
            } else {
                markNamespaceCName(fn->name);
            }
        }
        return decl;
    }

    // CON-04: `thread_local T name = init;` — storage class for variables only.
    // Functions are rejected at sema (SemInvalidThreadLocalOnFunction).
    if (check(TokenType::TOKEN_THREAD_LOCAL)) {
        advance();
        auto decl = parseDeclaration();
        // CON-04: `thread_local` applies to variables only; a function or other
        // declaration keeps the flag for sema to diagnose.
        std::function<void(DeclAST*)> markVar = [&markVar](DeclAST* d) {
            if (auto* var = dynamic_cast<VarDeclAST*>(d)) {
                var->isThreadLocal = true;
            } else if (auto* multi = dynamic_cast<MultiVarDeclAST*>(d)) {
                for (auto& sub : multi->decls) markVar(sub.get());
            } else if (auto* fn = dynamic_cast<FunctionDeclAST*>(d)) {
                fn->isThreadLocal = true;
            }
        };
        markVar(decl.get());
        return decl;
    }

    if (isRemovedTypeAliasStart()) {
        rejectRemovedTypeAlias();
        return nullptr;
    }

    if (isCompileOnlyStart()) {
        return parseCompileOnlyDeclaration();
    }

    // P1-03 / GEN-01 / PAR-21: `template<...>` 声明。
    if (check(TokenType::TOKEN_TEMPLATE)) {
        return parseTemplateDecl();
    }

    // P1-04 / CT-01: `compile_time.if` / `compile_time.static_assert` 顶层声明。
    if (isCompileTimeDeclStart()) {
        if (m_tokens[m_currentTokenPos + 2].lexeme == "if") {
            return parseCompileTimeIf();
        }
        return parseCompileTimeAssert();
    }

    // 新增：支持 using 类型别名
    if (check(TokenType::TOKEN_IDENTIFIER)) {
        std::string name = peek()->lexeme;
        if (name == "using") {
            size_t savedPos = m_currentTokenPos;
            advance(); // 消耗 "using"
            // 解析: using Name = Type;
            if (check(TokenType::TOKEN_IDENTIFIER)) {
                std::string aliasName = qualifyTypeDeclName(advance()->lexeme);
                if (match(TokenType::TOKEN_ASSIGN)) {
                    Type* aliasedType = parseTypeId();
                    if (aliasedType) {
                        match(TokenType::TOKEN_SEMICOLON);
                        TypeContext::instance().addTypedef(aliasName, aliasedType);
                        return std::make_unique<UsingDeclAST>(aliasName, aliasedType);
                    }
                }
            }
            // 如果解析失败，回退
            m_currentTokenPos = savedPos;
        }
    }

    if (check(TokenType::TOKEN_STRUCT)) {
        size_t savedPos = m_currentTokenPos;
        auto structDecl = parseStructDecl();
        if (!structDecl) return nullptr;

        // Register struct type in TypeContext if it has fields or nested
        // type declarations (AGG-11: a nested-only struct still names a type).
        if (!structDecl->fields.empty() || !structDecl->nestedTypes.empty() ||
            !structDecl->methods.empty() || structDecl->hasAccessSections) {
            const bool isClassRecord = isClassLikeRecord(*structDecl);
            if (isClassRecord) {
                // A `struct` with member functions or access sections routes
                // through the class type (same layout, methods, access).
                ClassType* classType = TypeContext::instance().getClass(structDecl->name);
                if (!classType) {
                    classType = new ClassType(structDecl->name);
                } else {
                    classType->fields.clear();
                }
                for (auto& field : structDecl->fields) {
                    classType->addField(field.name, field.type, field.bitWidth, field.annotations);
                    if (field.flexibleArray && !classType->fields.empty())
                        classType->fields.back().flexibleArray = true;
                }
                TypeContext::instance().addClass(structDecl->name, classType);
                if (!structDecl->isForwardDecl) classType->parsedDefinition = true;
            } else {
                // MOD-03: a definition completes the parse-time placeholder in
                // place, so aliases/pointers bound before it see the fields.
                StructType* structType = TypeContext::instance().getStruct(structDecl->name);
                if (!structType) {
                    structType = new StructType(structDecl->name);
                } else {
                    structType->fields.clear();
                }
                for (auto& field : structDecl->fields) {
                    structType->addField(field.name, field.type, field.bitWidth, field.annotations);
                    if (field.flexibleArray && !structType->fields.empty())
                        structType->fields.back().flexibleArray = true;
                }
                TypeContext::instance().addStruct(structDecl->name, structType);
                if (!structDecl->isForwardDecl) structType->parsedDefinition = true;
            }
        }

        // A record parser stops in one of three places:
        //  - at ';'          → complete definition, no attached declarator
        //  - at '}'          → definition followed by an attached declarator
        //  - without a body  → forward declaration (`struct S;`) or a record
        //                      type inside a declarator (`struct S f()`,
        //                      `struct S* p`, `struct S operator+(...)`).
        // Only the '}' shape may own an attached declarator; in the other two
        // shapes an identifier starts the *next* declaration.
        const bool hasBody = !structDecl->isForwardDecl;
        const bool consumedSemicolon = m_currentTokenPos > 0 &&
            m_tokens[m_currentTokenPos - 1].type == TokenType::TOKEN_SEMICOLON;
        if (!hasBody) {
            if (match(TokenType::TOKEN_SEMICOLON)) {
                // Forward declaration: register a placeholder so a later bare
                // use (`S* p;`) resolves before the definition appears.
                if (!TypeContext::instance().getStruct(structDecl->name))
                    TypeContext::instance().addStruct(
                        structDecl->name, new StructType(structDecl->name));
                return structDecl;
            }
            m_currentTokenPos = savedPos;
            return parseDeclarationAsType();
        }

        if (!consumedSemicolon &&
            (check(TokenType::TOKEN_IDENTIFIER) || check(TokenType::TOKEN_STAR) ||
             check(TokenType::TOKEN_LPAREN))) {
            const bool isClassRecord = isClassLikeRecord(*structDecl);
            Type* type = nullptr;
            if (isClassRecord) {
                type = TypeContext::instance().getClass(structDecl->name);
                if (!type) {
                    auto* classType = new ClassType(structDecl->name);
                    TypeContext::instance().addClass(structDecl->name, classType);
                    type = classType;
                }
            } else {
                type = TypeContext::instance().getStruct(structDecl->name);
                if (!type) {
                    auto* structType = new StructType(structDecl->name);
                    TypeContext::instance().addStruct(structDecl->name, structType);
                    type = structType;
                }
            }
            // Pure data structs keep the historic shape: the record AST is
            // consumed at parse time and the returned node is the variable.
            // Class-like structs (methods/access sections) must keep the type
            // definition on the same AST node so sema registers fields/methods/
            // access before the attached variable sees the type.
            if (isClassRecord) {
                structDecl->attachedVars = parseDeclarationFromBaseType(type);
                return structDecl;
            }
            return parseDeclarationFromBaseType(type);
        }

        return structDecl;
    }

    if (check(TokenType::TOKEN_CLASS)) {
        size_t savedPos = m_currentTokenPos;
        auto classDecl = parseClassDecl();
        if (!classDecl) return nullptr;

        // Register class type in TypeContext (methods will be added by semantic analyzer)
        auto existingType = TypeContext::instance().getClass(classDecl->name);
        if (!existingType) {
            auto* classType = new ClassType(classDecl->name);
            for (auto& field : classDecl->fields) {
                classType->addField(field.name, field.type);
                if (field.flexibleArray && !classType->fields.empty())
                    classType->fields.back().flexibleArray = true;
            }
            TypeContext::instance().addClass(classDecl->name, classType);
            existingType = classType;
        }
        if (existingType && !classDecl->isForwardDecl) existingType->parsedDefinition = true;

        // See the struct branch: only a body followed by a declarator owns the
        // next identifier. A bodiless `class C` is either a forward
        // declaration or a type inside a declarator (`class C f()`,
        // `class C* p`).
        const bool hasBody = !classDecl->isForwardDecl;
        const bool consumedSemicolon = m_currentTokenPos > 0 &&
            m_tokens[m_currentTokenPos - 1].type == TokenType::TOKEN_SEMICOLON;
        if (!hasBody) {
            if (match(TokenType::TOKEN_SEMICOLON)) return classDecl;
            m_currentTokenPos = savedPos;
            return parseDeclarationAsType();
        }

        if (!consumedSemicolon &&
            (check(TokenType::TOKEN_IDENTIFIER) || check(TokenType::TOKEN_STAR) ||
             check(TokenType::TOKEN_LPAREN))) {
            auto type = TypeContext::instance().getClass(classDecl->name);
            if (!type) {
                type = new ClassType(classDecl->name);
                TypeContext::instance().addClass(classDecl->name, static_cast<ClassType*>(type));
            }
            // PAR-12: 变量挂在 classDecl AST 上（不丢弃聚合 AST）——sema 先
            // 注册字段/方法/访问段，再处理变量；codegen 同理。此前直接返回
            // 变量导致字段/方法从未注册（`class { ... } c;` 成员不可用）。
            classDecl->attachedVars = parseDeclarationFromBaseType(type);
            return classDecl;
        }

        return classDecl;
    }

    if (check(TokenType::TOKEN_UNION)) {
        size_t savedPos = m_currentTokenPos;
        auto unionDecl = parseUnionDecl();
        if (!unionDecl) return nullptr;

        // Register union type in TypeContext if it has members or nested
        // type declarations (AGG-11: a nested-only union still names a type).
        if (!unionDecl->members.empty() || !unionDecl->nestedTypes.empty()) {
            // MOD-03: mirror the struct placeholder reuse.
            UnionType* unionType = TypeContext::instance().getUnion(unionDecl->name);
            if (!unionType) {
                unionType = new UnionType(unionDecl->name);
            } else {
                unionType->members.clear();
            }
            for (auto& member : unionDecl->members) {
                unionType->addMember(member.name, member.type, member.bitWidth);
                if (member.flexibleArray && !unionType->members.empty())
                    unionType->members.back().flexibleArray = true;
            }
            TypeContext::instance().addUnion(unionDecl->name, unionType);
            if (!unionDecl->isForwardDecl) unionType->parsedDefinition = true;
        }

        const bool hasBody = !unionDecl->isForwardDecl;
        const bool consumedSemicolon = m_currentTokenPos > 0 &&
            m_tokens[m_currentTokenPos - 1].type == TokenType::TOKEN_SEMICOLON;
        if (!hasBody) {
            if (match(TokenType::TOKEN_SEMICOLON)) {
                // Forward declaration: register a placeholder (mirrors struct).
                if (!TypeContext::instance().getUnion(unionDecl->name))
                    TypeContext::instance().addUnion(
                        unionDecl->name, new UnionType(unionDecl->name));
                return unionDecl;
            }
            m_currentTokenPos = savedPos;
            return parseDeclarationAsType();
        }

        if (!consumedSemicolon &&
            (check(TokenType::TOKEN_IDENTIFIER) || check(TokenType::TOKEN_STAR) ||
             check(TokenType::TOKEN_LPAREN))) {
            auto type = TypeContext::instance().getUnion(unionDecl->name);
            if (!type) {
                type = new UnionType(unionDecl->name);
                TypeContext::instance().addUnion(unionDecl->name, static_cast<UnionType*>(type));
            }
            return parseDeclarationFromBaseType(type);
        }

        return unionDecl;
    }

    if (check(TokenType::TOKEN_ENUM)) {
        size_t savedPos = m_currentTokenPos;
        auto enumDecl = parseEnumDecl();
        if (!enumDecl) return nullptr;

        // Register enum type in TypeContext if it has values
        if (!enumDecl->values.empty()) {
            auto enumType = new EnumType(enumDecl->name);
            for (auto& val : enumDecl->values) {
                enumType->addValue(val.first, val.second);
            }
            TypeContext::instance().addEnum(enumDecl->name, enumType);
        }

        // An enum parser never consumes the declaration's ';'. A bodiless
        // `enum E` is a forward declaration when followed by ';', otherwise a
        // type inside a declarator (`enum E f()`).
        const bool hasBody = !enumDecl->isForwardDecl;
        if (!hasBody) {
            if (match(TokenType::TOKEN_SEMICOLON)) return enumDecl;
            m_currentTokenPos = savedPos;
            return parseDeclarationAsType();
        }

        if (check(TokenType::TOKEN_IDENTIFIER) || check(TokenType::TOKEN_STAR) ||
            check(TokenType::TOKEN_LPAREN)) {
            auto type = TypeContext::instance().getEnum(enumDecl->name);
            if (!type) {
                type = new EnumType(enumDecl->name);
                TypeContext::instance().addEnum(enumDecl->name, static_cast<EnumType*>(type));
            }
            return parseDeclarationFromBaseType(type);
        }

        match(TokenType::TOKEN_SEMICOLON); // definition terminator
        return enumDecl;
    }

    return parseDeclarationAsType();
}

// Strip the pointer/array/slice/optional layers introduced by the first
// declarator so subsequent declarators in `int* a, b;` see the base type.
static Type* stripDeclaratorType(Type* type) {
    while (type) {
        switch (type->kind) {
            case TypeKind::Pointer:
                type = type->base;
                break;
            case TypeKind::Array:
                type = static_cast<ArrayType*>(type)->elementType;
                break;
            case TypeKind::Slice:
                type = static_cast<SliceType*>(type)->elementType;
                break;
            case TypeKind::Optional:
                type = static_cast<OptionalType*>(type)->elementType;
                break;
            case TypeKind::Function:
                type = static_cast<FunctionType*>(type)->returnType;
                break;
            default:
                return type;
        }
    }
    return type;
}

std::unique_ptr<DeclAST> Parser::parseDeclarationFromBaseType(Type* base,
                                                              bool isCompileTime) {
    // Parenthesized declarator: `(*fp)(int32)` / `(*p)[3]`.
    if (check(TokenType::TOKEN_LPAREN)) {
        std::string name;
        bool isFunction = false;
        std::vector<std::unique_ptr<ParamDeclAST>> params;
        Type* declType = parseFunctionPointerDeclarator(
            base, name, /*requireName=*/true, &isFunction, &params);
        if (!declType) return nullptr;
        if (isFunction) {
            std::unique_ptr<CompoundStmtAST> body;
            if (check(TokenType::TOKEN_LBRACE)) {
                body = parseCompoundStmt();
            } else if (!check(TokenType::TOKEN_SEMICOLON) && !eof()) {
                errorUnexpected("expected '{' or ';' after function declaration");
            }
            if (declType->kind != TypeKind::Function) return nullptr;
            auto* ft = static_cast<FunctionType*>(declType);
            return std::make_unique<FunctionDeclAST>(
                name, ft->returnType, params, body, isCompileTime, ft->isVarArg);
        }
        return parseVariableDeclList(declType, name, isCompileTime);
    }

    // Leading pointers: `*p`, `**p`, `*f()` (function returning pointer).
    Type* type = base;
    while (check(TokenType::TOKEN_STAR)) {
        advance();
        type = new Type(TypeKind::Pointer, type);
        while (check(TokenType::TOKEN_CONST) || check(TokenType::TOKEN_VOLATILE) ||
               check(TokenType::TOKEN_RESTRICT)) {
            const bool isConst = check(TokenType::TOKEN_CONST);
            const bool isVolatile = check(TokenType::TOKEN_VOLATILE);
            const bool isRestrict = check(TokenType::TOKEN_RESTRICT);
            advance();
            if (isConst) type->isConst = true;
            if (isVolatile) type->isVolatile = true;
            if (isRestrict) type->isRestrict = true;
        }
    }

    if (!check(TokenType::TOKEN_IDENTIFIER)) {
        errorUnexpected("expected declarator after record definition");
        return nullptr;
    }
    auto nameTok = advance();
    if (check(TokenType::TOKEN_LPAREN)) {
        return parseFunctionDecl(type, nameTok->lexeme, isCompileTime);
    }
    return parseVariableDeclList(type, nameTok->lexeme, isCompileTime);
}

std::unique_ptr<DeclAST> Parser::parseDeclarationAsType() {
    bool isCompileTime = false;
    consumeCompileTimeSpecifier(isCompileTime);

    // 双报防护（Task 5）：记录本次尝试前的诊断数——若 parseType 已诊断（如
    // 模板实参失败），不补报 unknown type。
    size_t errorsBefore = m_errors.size();
    std::optional<Token> startTok = peek();
    Type* type = parseType();
    if (!type) {
        // 未知类型名声明不再静默丢弃：起始 token 为标识符（或类型起始关键字）
        // 且本次尝试未新增诊断时，报 unknown type。
        if (startTok && startTok->type == TokenType::TOKEN_IDENTIFIER &&
            m_errors.size() == errorsBefore) {
            error(DiagnosticCode::SemUnknownType,
                  "unknown type '" + startTok->lexeme + "'", *startTok);
        }
        return nullptr;
    }

    // Check for operator keyword (operator overloading)
    if (check(TokenType::TOKEN_OPERATOR)) {
        advance(); // consume 'operator'
        auto opToken = peek();
        if (!opToken) {
            errorUnexpected("expected operator symbol after 'operator'");
            return nullptr;
        }
        // Map token type to operator string (token lexemes may be empty for single-char tokens)
        std::string opSym;
        bool symbolConsumed = false;
        switch (opToken->type) {
            case TokenType::TOKEN_PLUS:      opSym = "+"; break;
            case TokenType::TOKEN_MINUS:     opSym = "-"; break;
            case TokenType::TOKEN_STAR:      opSym = "*"; break;
            case TokenType::TOKEN_SLASH:     opSym = "/"; break;
            case TokenType::TOKEN_PERCENT:   opSym = "%"; break;
            case TokenType::TOKEN_EQ:        opSym = "=="; break;
            case TokenType::TOKEN_NOT_EQ:    opSym = "!="; break;
            case TokenType::TOKEN_LT:        opSym = "<"; break;
            case TokenType::TOKEN_GT:        opSym = ">"; break;
            case TokenType::TOKEN_LE:        opSym = "<="; break;
            case TokenType::TOKEN_GE:        opSym = ">="; break;
            case TokenType::TOKEN_AND:       opSym = "&&"; break;
            case TokenType::TOKEN_OR:        opSym = "||"; break;
            case TokenType::TOKEN_BIT_AND:   opSym = "&"; break;
            case TokenType::TOKEN_BIT_OR:    opSym = "|"; break;
            case TokenType::TOKEN_CARET:     opSym = "^"; break;
            case TokenType::TOKEN_LSHIFT:    opSym = "<<"; break;
            case TokenType::TOKEN_RSHIFT:    opSym = ">>"; break;
            case TokenType::TOKEN_TILDE:     opSym = "~"; break;
            case TokenType::TOKEN_NOT:       opSym = "!"; break;
            case TokenType::TOKEN_PLUS_PLUS: opSym = "++"; break;
            case TokenType::TOKEN_MINUS_MINUS: opSym = "--"; break;
            // SEM-17: `operator[]` / `operator()` — the symbol is a bracket
            // pair; consume it here, then parseFunctionDecl reads the real
            // parameter list.
            case TokenType::TOKEN_LBRACKET:
                advance(); // '['
                if (!match(TokenType::TOKEN_RBRACKET)) {
                    errorUnexpected("expected ']' after 'operator['");
                    return nullptr;
                }
                opSym = "[]";
                symbolConsumed = true;
                break;
            case TokenType::TOKEN_LPAREN:
                advance(); // '('
                if (!match(TokenType::TOKEN_RPAREN)) {
                    errorUnexpected("expected ')' after 'operator('");
                    return nullptr;
                }
                opSym = "()";
                symbolConsumed = true;
                break;
            default:
                errorUnexpected("expected operator symbol after 'operator'");
                return nullptr;
        }
        std::string opName = "operator" + opSym;
        if (!symbolConsumed) advance(); // consume the single-token operator symbol
        
        if (!check(TokenType::TOKEN_LPAREN)) {
            errorUnexpected("expected '(' after operator name");
            return nullptr;
        }
        return parseFunctionDecl(type, opName, isCompileTime);
    }

    // Parenthesized pointer declarator: function pointer, pointer-to-array,
    // array-of-function-pointers, or function returning a function pointer.
    {
        std::string fpName;
        bool isFunction = false;
        std::vector<std::unique_ptr<ParamDeclAST>> fnParams;
        if (Type* declType = parseFunctionPointerDeclarator(
                type, fpName, /*requireName=*/true, &isFunction, &fnParams)) {
            if (isFunction) {
                if (declType->kind != TypeKind::Function) return nullptr;
                auto* ft = static_cast<FunctionType*>(declType);
                std::unique_ptr<CompoundStmtAST> body;
                if (check(TokenType::TOKEN_LBRACE)) {
                    body = parseCompoundStmt();
                } else if (!check(TokenType::TOKEN_SEMICOLON) && !eof()) {
                    errorUnexpected("expected '{' or ';' after function declaration");
                }
                return std::make_unique<FunctionDeclAST>(
                    fpName, ft->returnType, fnParams, body, isCompileTime, ft->isVarArg);
            }
            return parseVariableDeclList(declType, fpName, isCompileTime);
        }
    }

    // PAR-03: out-of-class method definition `RetType Class::method(params)`.
    // The class body precedes it in the TU (C++-style declaration/definition
    // split); sema binds the body to the method declared in that class.
    if (check(TokenType::TOKEN_IDENTIFIER)) {
        size_t saved = m_currentTokenPos;
        auto classTok = advance();
        if (check(TokenType::TOKEN_COLON_COLON)) {
            advance(); // '::'
            if (check(TokenType::TOKEN_IDENTIFIER)) {
                auto methodTok = advance();
                if (check(TokenType::TOKEN_LPAREN)) {
                    auto fn = parseFunctionDecl(type, methodTok->lexeme, isCompileTime);
                    if (fn) fn->className = classTok->lexeme;
                    return fn;
                }

                // Out-of-class static data definition: `int32 S::x = 1;`.
                // Keep the member name and class separately so sema can bind
                // it to the in-class static member before codegen.
                auto decl = parseVariableDecl(type, methodTok->lexeme, isCompileTime);
                if (auto* vd = dynamic_cast<VarDeclAST*>(decl.get())) {
                    vd->isOutOfClassStatic = true;
                    vd->staticClass = classTok->lexeme;
                    vd->staticMember = methodTok->lexeme;
                    expect(TokenType::TOKEN_SEMICOLON,
                           "expected ';' after static data definition");
                } else if (decl) {
                    error("out-of-class static data definition must be a scalar variable",
                          *methodTok);
                }
                return decl;
            } else if (check(TokenType::TOKEN_OPERATOR)) {
                advance(); // 'operator'
                auto opTok = peek();
                std::string sym;
                if (opTok) {
                    switch (opTok->type) {
                        case TokenType::TOKEN_PLUS:        sym = "+"; advance(); break;
                        case TokenType::TOKEN_MINUS:       sym = "-"; advance(); break;
                        case TokenType::TOKEN_STAR:        sym = "*"; advance(); break;
                        case TokenType::TOKEN_SLASH:       sym = "/"; advance(); break;
                        case TokenType::TOKEN_PERCENT:     sym = "%"; advance(); break;
                        case TokenType::TOKEN_EQ:          sym = "=="; advance(); break;
                        case TokenType::TOKEN_NOT_EQ:      sym = "!="; advance(); break;
                        case TokenType::TOKEN_LT:          sym = "<"; advance(); break;
                        case TokenType::TOKEN_GT:          sym = ">"; advance(); break;
                        case TokenType::TOKEN_LE:          sym = "<="; advance(); break;
                        case TokenType::TOKEN_GE:          sym = ">="; advance(); break;
                        case TokenType::TOKEN_AND:         sym = "&&"; advance(); break;
                        case TokenType::TOKEN_OR:          sym = "||"; advance(); break;
                        case TokenType::TOKEN_BIT_AND:     sym = "&"; advance(); break;
                        case TokenType::TOKEN_BIT_OR:      sym = "|"; advance(); break;
                        case TokenType::TOKEN_CARET:       sym = "^"; advance(); break;
                        case TokenType::TOKEN_LSHIFT:      sym = "<<"; advance(); break;
                        case TokenType::TOKEN_RSHIFT:      sym = ">>"; advance(); break;
                        case TokenType::TOKEN_TILDE:       sym = "~"; advance(); break;
                        case TokenType::TOKEN_NOT:         sym = "!"; advance(); break;
                        case TokenType::TOKEN_PLUS_PLUS:   sym = "++"; advance(); break;
                        case TokenType::TOKEN_MINUS_MINUS: sym = "--"; advance(); break;
                        case TokenType::TOKEN_LBRACKET:
                            advance();
                            if (!match(TokenType::TOKEN_RBRACKET)) {
                                errorUnexpected("expected ']' after 'operator['");
                                return nullptr;
                            }
                            sym = "[]";
                            break;
                        case TokenType::TOKEN_LPAREN:
                            advance();
                            if (!match(TokenType::TOKEN_RPAREN)) {
                                errorUnexpected("expected ')' after 'operator('");
                                return nullptr;
                            }
                            sym = "()";
                            break;
                        default:
                            break;
                    }
                }
                if (sym.empty() || !check(TokenType::TOKEN_LPAREN)) {
                    errorUnexpected("expected operator symbol and parameter list");
                    return nullptr;
                }
                auto fn = parseFunctionDecl(type, "operator" + sym, isCompileTime);
                if (fn) fn->className = classTok->lexeme;
                return fn;
            }
        }
        m_currentTokenPos = saved;
    }

    if (!check(TokenType::TOKEN_IDENTIFIER)) return nullptr;
    auto nameTok = advance();

    // Function declaration/definition: type name '('
    if (check(TokenType::TOKEN_LPAREN)) {
        auto first = parseFunctionDecl(type, nameTok->lexeme, isCompileTime,
                                       /*allowComma=*/true);
        if (!first) return nullptr;
        if (!check(TokenType::TOKEN_COMMA)) return first;

        // C permits several function declarators in one declaration:
        // `int32 f(), g();`. A function *definition* cannot be part of such a
        // list, so reject `int32 f() { ... }, g();`.
        if (first->body) {
            errorUnexpected("function definition cannot be followed by ','");
            return first;
        }
        std::vector<std::unique_ptr<DeclAST>> fns;
        fns.push_back(std::move(first));
        while (match(TokenType::TOKEN_COMMA)) {
            if (!check(TokenType::TOKEN_IDENTIFIER)) {
                errorUnexpected("expected function name after ','");
                break;
            }
            auto extraName = advance();
            if (!check(TokenType::TOKEN_LPAREN)) {
                errorUnexpected("expected '(' after function name");
                break;
            }
            auto fn = parseFunctionDecl(type, extraName->lexeme, isCompileTime,
                                        /*allowComma=*/true);
            if (!fn) break;
            fns.push_back(std::move(fn));
        }
        expect(TokenType::TOKEN_SEMICOLON, "expected ';' after function declarations");
        return std::make_unique<MultiVarDeclAST>(std::move(fns));
    }

    // Variable declaration, possibly with several comma-separated declarators.
    return parseVariableDeclList(type, nameTok->lexeme, isCompileTime);
}

std::unique_ptr<DeclAST> Parser::parseVariableDeclList(Type* type, const std::string& firstName, bool isCompileTime) {
    auto first = parseVariableDecl(type, firstName, isCompileTime);
    if (!check(TokenType::TOKEN_COMMA)) {
        expect(TokenType::TOKEN_SEMICOLON, "expected ';' after variable declaration");
        return first;
    }

    std::vector<std::unique_ptr<DeclAST>> vars;
    vars.push_back(std::move(first));

    // `int* a, b;` — the pointer belongs to the first declarator only, so
    // subsequent declarators start from the unqualified base type.
    Type* baseType = stripDeclaratorType(type);
    while (match(TokenType::TOKEN_COMMA)) {
        // Parenthesized declarator after the comma:
        // `int (*f)(int), (*g)(int);` or `int (*p)[3], (*q)[3];`.
        if (check(TokenType::TOKEN_LPAREN)) {
            std::string extraName;
            bool isFunction = false;
            std::vector<std::unique_ptr<ParamDeclAST>> params;
            Type* varType = parseFunctionPointerDeclarator(
                baseType, extraName, /*requireName=*/true, &isFunction, &params);
            if (!varType) break;
            if (isFunction) {
                error("function declaration cannot follow a variable declarator", *peek());
                break;
            }
            auto extra = parseVariableDecl(varType, extraName, isCompileTime);
            if (!extra) break;
            vars.push_back(std::move(extra));
            continue;
        }

        Type* varType = baseType;
        while (check(TokenType::TOKEN_STAR)) {
            advance();
            varType = new Type(TypeKind::Pointer, varType);
            while (check(TokenType::TOKEN_CONST) || check(TokenType::TOKEN_VOLATILE) ||
                   check(TokenType::TOKEN_RESTRICT)) {
                const bool isConst = check(TokenType::TOKEN_CONST);
                const bool isVolatile = check(TokenType::TOKEN_VOLATILE);
                const bool isRestrict = check(TokenType::TOKEN_RESTRICT);
                advance();
                if (isConst) varType->isConst = true;
                if (isVolatile) varType->isVolatile = true;
                if (isRestrict) varType->isRestrict = true;
            }
        }
        while (check(TokenType::TOKEN_CONST) || check(TokenType::TOKEN_VOLATILE) ||
               check(TokenType::TOKEN_ATOMIC)) {
            advance();
        }
        if (!check(TokenType::TOKEN_IDENTIFIER)) {
            errorUnexpected("expected identifier after ',' in declaration");
            break;
        }
        auto extraName = advance();
        auto extra = parseVariableDecl(varType, extraName->lexeme, isCompileTime);
        if (!extra) break;
        vars.push_back(std::move(extra));
    }

    expect(TokenType::TOKEN_SEMICOLON, "expected ';' after variable declaration");
    return std::make_unique<MultiVarDeclAST>(std::move(vars));
}

Type* Parser::parseFunctionPointerDeclarator(Type* returnType, std::string& outName,
                                             bool requireName,
                                             bool* outIsFunction,
                                             std::vector<std::unique_ptr<ParamDeclAST>>* outFunctionParams) {
    // SG-05: one recursive declarator engine instead of the previous
    // single-parenthesis special case. `ops` is the constructor list in
    // identifier-outward order; building the type walks it in reverse.
    struct Op {
        enum Kind { Pointer, Array, Function } kind;
        int size = 0;
        bool varArg = false;
        std::vector<std::unique_ptr<ParamDeclAST>> params;
    };

    outName.clear();
    if (outIsFunction) *outIsFunction = false;
    if (!check(TokenType::TOKEN_LPAREN)) return nullptr;

    const size_t startPos = m_currentTokenPos;

    auto groupingStartsHere = [this]() -> bool {
        if (!check(TokenType::TOKEN_LPAREN)) return false;
        size_t i = m_currentTokenPos + 1;
        while (i < m_tokens.size() &&
               (m_tokens[i].type == TokenType::TOKEN_CONST ||
                m_tokens[i].type == TokenType::TOKEN_VOLATILE ||
                m_tokens[i].type == TokenType::TOKEN_RESTRICT)) {
            ++i;
        }
        if (i >= m_tokens.size()) return false;
        return m_tokens[i].type == TokenType::TOKEN_STAR ||
               m_tokens[i].type == TokenType::TOKEN_IDENTIFIER ||
               m_tokens[i].type == TokenType::TOKEN_LPAREN;
    };

    auto parseSuffixes = [&](std::vector<Op>& ops) -> bool {
        while (!eof()) {
            if (check(TokenType::TOKEN_LBRACKET)) {
                advance();
                auto numTok = match(TokenType::TOKEN_NUMBER);
                if (!numTok) {
                    errorUnexpected("array size must be an integer literal");
                    return false;
                }
                int size = static_cast<int>(
                    static_cast<long long>(std::get<unsigned __int128>(numTok->value)));
                if (!expect(TokenType::TOKEN_RBRACKET, "expected ']' after array size"))
                    return false;
                Op op; op.kind = Op::Array; op.size = size;
                ops.push_back(std::move(op));
                continue;
            }
            if (check(TokenType::TOKEN_LPAREN)) {
                advance();
                Op op; op.kind = Op::Function;
                if (!check(TokenType::TOKEN_RPAREN)) {
                    while (true) {
                        if (check(TokenType::TOKEN_VOID)) {
                            size_t v = m_currentTokenPos;
                            advance();
                            if (check(TokenType::TOKEN_RPAREN)) break;
                            m_currentTokenPos = v;
                        }
                        if (check(TokenType::TOKEN_ELLIPSIS)) {
                            op.varArg = true;
                            advance();
                            break;
                        }
                        auto param = parseParamDecl();
                        if (!param) return false;
                        op.params.push_back(std::move(param));
                        if (!match(TokenType::TOKEN_COMMA)) break;
                    }
                }
                if (!expect(TokenType::TOKEN_RPAREN, "expected ')' after parameter list"))
                    return false;
                ops.push_back(std::move(op));
                continue;
            }
            break;
        }
        return true;
    };

    std::function<bool(std::vector<Op>&)> parseDecl =
        [&](std::vector<Op>& ops) -> bool {
        int pointerCount = 0;
        while (check(TokenType::TOKEN_STAR)) {
            advance();
            ++pointerCount;
            while (check(TokenType::TOKEN_CONST) ||
                   check(TokenType::TOKEN_VOLATILE) ||
                   check(TokenType::TOKEN_RESTRICT)) {
                advance();
            }
        }

        if (groupingStartsHere()) {
            advance(); // '('
            std::vector<Op> inner;
            if (!parseDecl(inner)) return false;
            if (!expect(TokenType::TOKEN_RPAREN, "expected ')' in declarator"))
                return false;
            for (auto& op : inner) ops.push_back(std::move(op));
        } else if (check(TokenType::TOKEN_IDENTIFIER)) {
            outName = advance()->lexeme;
        } else if (requireName) {
            return false;
        }

        if (!parseSuffixes(ops)) return false;

        // Prefix `*` applies after the declarator's own suffixes (e.g.
        // `*f()` = function returning pointer; `(*f)()` is handled by the
        // grouping branch above and therefore places the pointer first).
        for (int i = 0; i < pointerCount; ++i) {
            Op p; p.kind = Op::Pointer;
            ops.push_back(std::move(p));
        }
        return true;
    };

    std::vector<Op> ops;
    if (!parseDecl(ops)) {
        m_currentTokenPos = startPos;
        outName.clear();
        return nullptr;
    }
    // A top-level abstract type-id has no name; callers that need a name pass
    // requireName=true. For requireName=false the empty name is valid.
    if (requireName && outName.empty()) {
        m_currentTokenPos = startPos;
        return nullptr;
    }

    const bool entityIsFunction = !ops.empty() && ops.front().kind == Op::Function;
    if (outIsFunction) *outIsFunction = entityIsFunction;
    if (entityIsFunction && outFunctionParams) {
        *outFunctionParams = std::move(ops.front().params);
    }

    Type* type = returnType;
    for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
        if (it->kind == Op::Pointer) {
            type = new Type(TypeKind::Pointer, type);
        } else if (it->kind == Op::Array) {
            type = new ArrayType(type, it->size);
        } else {
            std::vector<Type*> pts;
            for (auto& p : it->params) pts.push_back(p->type);
            type = new FunctionType(type, std::move(pts), it->varArg);
        }
    }
    return type;
}

Type* Parser::parseAbstractFunctionSuffix(Type* returnType) {
    if (!check(TokenType::TOKEN_LPAREN)) return nullptr;
    size_t saved = m_currentTokenPos;
    advance(); // '('
    std::vector<Type*> paramTypes;
    bool isVarArg = false;
    if (!check(TokenType::TOKEN_RPAREN)) {
        while (true) {
            if (check(TokenType::TOKEN_VOID)) {
                size_t v = m_currentTokenPos;
                advance();
                if (check(TokenType::TOKEN_RPAREN)) break;
                m_currentTokenPos = v;
            }
            if (check(TokenType::TOKEN_ELLIPSIS)) {
                isVarArg = true;
                advance();
                break;
            }
            auto param = parseParamDecl();
            if (!param) {
                m_currentTokenPos = saved;
                return nullptr;
            }
            paramTypes.push_back(param->type);
            if (!match(TokenType::TOKEN_COMMA)) break;
        }
    }
    if (!expect(TokenType::TOKEN_RPAREN, "expected ')' after function type parameters")) {
        m_currentTokenPos = saved;
        return nullptr;
    }
    return new FunctionType(returnType, std::move(paramTypes), isVarArg);
}

Type* Parser::parseFunctionPointerType(Type* returnType) {
    if (!expect(TokenType::TOKEN_LPAREN, "expected '(' after function pointer name")) return nullptr;

    std::vector<Type*> paramTypes;
    bool isVarArg = false;

    if (!check(TokenType::TOKEN_RPAREN)) {
        while (true) {
            // See parseFunctionDecl: `void)` means no parameters, `void*` does not.
            if (check(TokenType::TOKEN_VOID)) {
                size_t saved = m_currentTokenPos;
                advance();
                if (check(TokenType::TOKEN_RPAREN)) {
                    break;
                }
                m_currentTokenPos = saved;
            }
            if (check(TokenType::TOKEN_ELLIPSIS)) {
                isVarArg = true;
                advance();
                break;
            }
            // Reuse the parameter parser so nested function-pointer parameters
            // (callbacks taking callbacks) decay the same way.
            auto param = parseParamDecl();
            if (!param) return nullptr;
            paramTypes.push_back(param->type);
            if (!match(TokenType::TOKEN_COMMA)) break;
        }
    }

    if (!expect(TokenType::TOKEN_RPAREN, "expected ')' after function pointer parameters")) return nullptr;
    return new FunctionType(returnType, paramTypes, isVarArg);
}

std::unique_ptr<FunctionDeclAST> Parser::parseFunctionDecl(Type* returnType, const std::string& name,
                                                          bool isCompileTime, bool allowComma) {
    if (!expect(TokenType::TOKEN_LPAREN, "expected '(' after function name")) return nullptr;

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    bool isVarArg = false;

    if (!check(TokenType::TOKEN_RPAREN)) {
        while (true) {
            // `void` denotes an empty parameter list only when followed by ')'.
            // `void*` is an ordinary parameter whose type starts with void.
            if (check(TokenType::TOKEN_VOID)) {
                size_t saved = m_currentTokenPos;
                advance();
                if (check(TokenType::TOKEN_RPAREN)) {
                    break;
                }
                m_currentTokenPos = saved;
            }

            auto param = parseParamDecl();
            if (!param) return nullptr;
            params.push_back(std::move(param));

            if (!match(TokenType::TOKEN_COMMA)) break;

            if (check(TokenType::TOKEN_ELLIPSIS)) {
                isVarArg = true;
                advance();
                break;
            }
        }
    }

    if (!expect(TokenType::TOKEN_RPAREN, "expected ')' after parameter list")) return nullptr;

    // Check for function body
    std::unique_ptr<CompoundStmtAST> body;
    if (check(TokenType::TOKEN_LBRACE)) {
        body = parseCompoundStmt();
    } else if (!check(TokenType::TOKEN_SEMICOLON) &&
               !(allowComma && check(TokenType::TOKEN_COMMA)) && !eof()) {
        errorUnexpected("expected '{' or ';' after function declaration");
    }

    return std::make_unique<FunctionDeclAST>(name, returnType, params, body, isCompileTime, isVarArg);
}

std::unique_ptr<DeclAST> Parser::parseVariableDecl(Type* type, const std::string& name, bool isCompileTime) {
    if (isCompileTime) m_compileTimeVarNames.insert(name);
    // compile_time implies read-only. Never mutate a shared scalar singleton.
    if (isCompileTime) {
        bool scalar = false;
        switch (type ? type->kind : TypeKind::Void) {
            case TypeKind::Char: case TypeKind::Bool:
            case TypeKind::Int8: case TypeKind::Int16:
            case TypeKind::Int32: case TypeKind::Int64:
            case TypeKind::Int128:
            case TypeKind::UInt8: case TypeKind::UInt16:
            case TypeKind::UInt32: case TypeKind::UInt64:
            case TypeKind::UInt128:
            case TypeKind::ISize: case TypeKind::USize:
            case TypeKind::Float16: case TypeKind::Float32:
            case TypeKind::Float64: case TypeKind::Float128:
                scalar = true;
                break;
            default:
                break;
        }
        if (scalar) {
            auto* copy = new Type(type->kind, type->base);
            copy->isConst = true;
            copy->isVolatile = type->isVolatile;
            copy->isAtomic = type->isAtomic;
            copy->isRestrict = type->isRestrict;
            type = copy;
        }
        // Aggregate/array compile_time variables are read-only via the
        // declaration/symbol flag, never by mutating the shared Type object.
    }

    std::unique_ptr<ExprAST> init;

    // Check for array declaration: name[size][size]... (TYP-11: the dimension
    // chain builds a nested ArrayType right-to-left — `T a[2][3]` is an
    // array of 2 arrays of 3 T, matching C and LLVM [2 x [3 x T]]).
    if (check(TokenType::TOKEN_LBRACKET)) {
        std::vector<int> dims;
        std::vector<bool> explicitDim;
        std::vector<std::string> dimNames;
        while (check(TokenType::TOKEN_LBRACKET)) {
            advance();
            int size = 0;
            bool hasNumber = false;
            std::string dimName;
            // VLA is a deliberate non-feature: runtime-length allocation is
            // unbounded stack growth. Only compile-time integer constants
            // (literals, enum constants, const/compile_time integers) are valid.
            if (!parseArrayDimension(size, hasNumber,
                                     dims.empty() ? &dimName : nullptr)) {
                return nullptr;
            }
            if (!expect(TokenType::TOKEN_RBRACKET, "expected ']' after array size")) {
                return nullptr;
            }
            dims.push_back(size);
            explicitDim.push_back(hasNumber);
            dimNames.push_back(dimName);
        }
        // C constraint: only the first dimension may be omitted (inferred
        // from the initializer). A missing inner dimension would silently
        // produce a zero-length row and out-of-bounds subscripts.
        for (size_t i = 1; i < dims.size(); ++i) {
            if (!explicitDim[i]) {
                errorUnexpected("expected array dimension: only the first dimension may be empty");
                return nullptr;
            }
        }

        // Check for initializer. Parse an assignment-expression (minPrec 2) so
        // that a following `,` starts the next declarator instead of becoming
        // part of a comma expression.
        if (check(TokenType::TOKEN_ASSIGN)) {
            advance();
            init = parseExpr(2);
        }

        Type* elemType = type;
        for (size_t i = dims.size() - 1; i >= 1; --i) {
            elemType = new ArrayType(elemType, dims[i]);
        }
        auto arrayDecl = std::make_unique<ArrayDeclAST>(name, elemType, dims[0],
                                                        std::move(init));
        if (!dimNames.empty()) arrayDecl->sizeName = dimNames[0];
        return arrayDecl;
    }

    // Check for initializer: = expr
    if (check(TokenType::TOKEN_ASSIGN)) {
        advance();
        init = parseExpr(2);
    }

    // Remember const/constexpr integer initializers so later array dimensions
    // and bit-field widths can use them as compile-time constants. Evaluate
    // before moving `init` into the declaration node.
    if ((isCompileTime || type->isConst) && isIntegerScalarType(type) && init) {
        long long value = 0;
        auto lookup = [this](const std::string& n, long long& out) {
            return lookupConstValue(n, out);
        };
        if (evalIntConstExpr(init.get(), value, lookup)) {
            registerConstValue(name, value);
        }
    }

    auto decl = std::make_unique<VarDeclAST>(name, type, std::move(init), isCompileTime);
    // Snapshot source-order completeness. isComplete is only filled by sema;
    // a later definition must not validate an earlier by-value use.
    Type* vt = type;
    while (vt && vt->kind == TypeKind::Typedef)
        vt = static_cast<TypedefType*>(vt)->aliasedType;
    while (vt && vt->kind == TypeKind::Array)
        vt = static_cast<ArrayType*>(vt)->elementType;
    while (vt && vt->kind == TypeKind::Typedef)
        vt = static_cast<TypedefType*>(vt)->aliasedType;
    if (vt) {
        if (vt->kind == TypeKind::Struct)
            decl->typeIncompleteAtParse = !static_cast<StructType*>(vt)->parsedDefinition &&
                                          static_cast<StructType*>(vt)->name.rfind("__anon_", 0) != 0;
        else if (vt->kind == TypeKind::Class)
            decl->typeIncompleteAtParse = !static_cast<ClassType*>(vt)->parsedDefinition &&
                                          static_cast<ClassType*>(vt)->name.rfind("__anon_", 0) != 0;
        else if (vt->kind == TypeKind::Union)
            decl->typeIncompleteAtParse = !static_cast<UnionType*>(vt)->parsedDefinition &&
                                          static_cast<UnionType*>(vt)->name.rfind("__anon_", 0) != 0;
    }

    return decl;
}

std::unique_ptr<ParamDeclAST> Parser::parseParamDecl() {
    // P1-05 / ANN-01: 参数注解（[[nonnull]] 等）。
    auto anns = parseAnnotations();
    Type* type = parseType();
    if (!type) return nullptr;

    // Parameter may itself be a function pointer: `int (*cb)(int, int)`.
    std::string name;
    if (Type* ptrType = parseFunctionPointerDeclarator(type, name, /*requireName=*/false)) {
        auto fpParam = std::make_unique<ParamDeclAST>(name, ptrType);
        fpParam->annotations = std::move(anns);
        return fpParam;
    }

    // PAR-08: parseType now accepts abstract `T[N]` (needed for compound
    // literals `(int32[3]){...}`). An unnamed array parameter `int32[2]`
    // must stay rejected (TYP-11 pin) — named array params desugar below.
    {
        Type* t = type;
        while (t && t->kind == TypeKind::Typedef)
            t = static_cast<TypedefType*>(t)->aliasedType;
        if (t && t->kind == TypeKind::Array) {
            errorUnexpected("array parameter requires the `T name[N]` form");
            return nullptr;
        }
    }

    if (check(TokenType::TOKEN_IDENTIFIER)) {
        name = advance()->lexeme;

        // C parameter adjustment: a function type in a parameter list decays
        // to a pointer to function. `int32 f(int32 cb(int32))` is equivalent
        // to `int32 f(int32 (*cb)(int32))`.
        if (check(TokenType::TOKEN_LPAREN)) {
            Type* fn = parseAbstractFunctionSuffix(type);
            if (!fn) return nullptr;
            type = new Type(TypeKind::Pointer, fn);
        }

        // TYP-11 D2: `T name[N]` 参数语法去糖为 slice `T[]`（[N] 为文档性
        // 标注，长度不静态检查——slice 长度本就动态，见
        // docs/superpowers/specs/2026-10-01-typ11-array-params-design.md）。
        // 多维扩展：`T name[][K]` 去糖为行 slice `Slice<ArrayType(T, K)>`，
        // 仅首维可缺省（内维缺省拒绝），见
        // docs/superpowers/specs/2026-10-04-multidim-array-params-design.md。
        if (check(TokenType::TOKEN_LBRACKET)) {
            std::vector<int> dims;
            std::vector<bool> explicitDim;
            while (check(TokenType::TOKEN_LBRACKET)) {
                advance();
                // SG-06: C99 array-parameter qualifiers. `static` means the
                // caller must provide at least N elements; the size itself is
                // still documentation because parameters decay to slices.
                while (check(TokenType::TOKEN_STATIC) ||
                       check(TokenType::TOKEN_CONST) ||
                       check(TokenType::TOKEN_VOLATILE) ||
                       check(TokenType::TOKEN_RESTRICT)) {
                    advance();
                }
                int dimSize = 0;
                bool hasNumber = false;
                // Same deliberate non-feature as declarations: a runtime
                // parameter size is documentation we cannot check.
                if (!parseArrayDimension(dimSize, hasNumber)) return nullptr;
                if (!expect(TokenType::TOKEN_RBRACKET, "expected ']' after array parameter size")) {
                    return nullptr;
                }
                dims.push_back(dimSize); // 首维数值仅文档性；内维数值构成行类型
                explicitDim.push_back(hasNumber);
            }
            // C 约束：仅首维可缺省（由初始化器/实参推断）。内维缺省会静默
            // 产生零长度行与越界下标，解析期拒绝。
            for (size_t i = 1; i < dims.size(); ++i) {
                if (!explicitDim[i]) {
                    errorUnexpected("expected array dimension: only the first dimension may be empty");
                    return nullptr;
                }
            }
            // 从右向左：`T name[][3]` = Slice<ArrayType(T, 3)>（C 语义，
            // 与 parseVariableDecl 的声明维度链同构）。
            Type* elemType = type;
            for (size_t i = dims.size() - 1; i >= 1; --i) {
                elemType = new ArrayType(elemType, dims[i]);
            }
            type = TypeContext::instance().getSliceType(elemType);
        }
    }

    auto param = std::make_unique<ParamDeclAST>(name, type);
    param->annotations = std::move(anns);
    // FUN-09: default parameter `T name = const-expr`.
    if (match(TokenType::TOKEN_ASSIGN)) {
        param->defaultValue = parseExpr();
        if (param->defaultValue) {
            param->hasDefault = true;
        } else {
            errorUnexpected("expected constant expression after '=' in default parameter");
        }
    }
    return param;
}

// P1-03 / GEN-01 / PAR-21: 解析 `template<...>` 声明。
// 参数列表 → 模板体（函数/struct/class/using 别名；union/enum 拒绝）。
// 定义处只 parse：参数名进入 m_templateScopes，模板体内的 `T`/`N` 由此解析。
static bool tokenStartsDeclarationType(TokenType type);

bool Parser::isCompileTimeModifierStart() const {
    if (eof() || peek()->type != TokenType::TOKEN_IDENTIFIER ||
        peek()->lexeme != "compile_time") {
        return false;
    }
    if (m_currentTokenPos + 1 >= m_tokens.size()) return false;
    return tokenStartsDeclarationType(m_tokens[m_currentTokenPos + 1].type);
}

static bool tokenStartsDeclarationType(TokenType type) {
    switch (type) {
        case TokenType::TOKEN_CHAR_KW:
        case TokenType::TOKEN_VOID:
        case TokenType::TOKEN_BOOL:
        case TokenType::TOKEN_AUTO:
        case TokenType::TOKEN_STRUCT:
        case TokenType::TOKEN_UNION:
        case TokenType::TOKEN_ENUM:
        case TokenType::TOKEN_CLASS:
        case TokenType::TOKEN_CONST:
        case TokenType::TOKEN_VOLATILE:
        case TokenType::TOKEN_ATOMIC:
        case TokenType::TOKEN_STATIC:
        case TokenType::TOKEN_EXTERN:
        case TokenType::TOKEN_THREAD_LOCAL:
        case TokenType::TOKEN_IDENTIFIER:
        case TokenType::TOKEN_INT8:
        case TokenType::TOKEN_INT16:
        case TokenType::TOKEN_INT32:
        case TokenType::TOKEN_INT64:
        case TokenType::TOKEN_INT128:
        case TokenType::TOKEN_UINT8:
        case TokenType::TOKEN_UINT16:
        case TokenType::TOKEN_UINT32:
        case TokenType::TOKEN_UINT64:
        case TokenType::TOKEN_UINT128:
        case TokenType::TOKEN_CHAR8:
        case TokenType::TOKEN_CHAR16:
        case TokenType::TOKEN_CHAR32:
        case TokenType::TOKEN_ISIZE:
        case TokenType::TOKEN_USIZE:
        case TokenType::TOKEN_FLOAT16:
        case TokenType::TOKEN_FLOAT32:
        case TokenType::TOKEN_FLOAT64:
        case TokenType::TOKEN_FLOAT128:
            return true;
        default:
            return false;
    }
}

bool Parser::isCompileOnlyStart() const {
    if (eof() || peek()->type != TokenType::TOKEN_IDENTIFIER ||
        peek()->lexeme != "compile_only") {
        return false;
    }
    if (m_currentTokenPos + 1 >= m_tokens.size()) return false;
    return tokenStartsDeclarationType(m_tokens[m_currentTokenPos + 1].type);
}

std::unique_ptr<DeclAST> Parser::parseCompileOnlyDeclaration() {
    Token spec = *peek();
    advance(); // compile_only
    auto decl = parseDeclaration();
    if (auto* fn = dynamic_cast<FunctionDeclAST*>(decl.get())) {
        fn->isCompileOnly = true;
        return decl;
    }
    error("compile_only is only valid on functions", spec);
    return decl;
}

bool Parser::consumeCompileTimeSpecifier(bool& isCompileTime) {
    if (check(TokenType::TOKEN_CONSTEXPR)) {
        Token tok = *peek();
        advance();
        error("'constexpr' has been removed; use 'compile_time' for functions/variables, "
              "or 'compile_only' for functions that must not exist at runtime",
              tok);
        isCompileTime = true;
        return true;
    }
    if (isCompileTimeModifierStart()) {
        advance();
        isCompileTime = true;
        if (check(TokenType::TOKEN_STATIC) || check(TokenType::TOKEN_EXTERN) ||
            check(TokenType::TOKEN_THREAD_LOCAL)) {
            Token storage = *peek();
            advance();
            error("compile_time cannot be combined with a storage class", storage);
        }
        return true;
    }
    return false;
}

// P1-04 / CT-01: `compile_time` 非关键字——仅当前瞻出 `compile_time` + `.` +
// 成员 ∈ {if, static_assert} 时才走编译期声明路径；只 peek 不消耗。
bool Parser::isCompileTimeDeclStart() const {
    if (eof() || peek()->lexeme != "compile_time") {
        return false;
    }
    if (m_currentTokenPos + 2 >= m_tokens.size()) {
        return false;
    }
    const Token& dot = m_tokens[m_currentTokenPos + 1];
    if (dot.type != TokenType::TOKEN_DOT) {
        return false;
    }
    const Token& member = m_tokens[m_currentTokenPos + 2];
    if (member.type == TokenType::TOKEN_IF && member.lexeme == "if") return true;
    if (member.type != TokenType::TOKEN_IDENTIFIER) return false;
    static const std::unordered_set<std::string> kDeclMembers = {
        "static_assert", "fail", "warn", "debug",
        "assert_eq", "assert_ne", "assert_size", "assert_align", "assert_offset",
    };
    return kDeclMembers.count(member.lexeme) != 0;
}

// P1-04 / CT-01: `compile_time.if (cond) { decls... } [else { decls... }]`。
// 两个分支都 parse（spec compile_time.md §3 钉死），选择在 sema。
std::unique_ptr<DeclAST> Parser::parseCompileTimeIf() {
    Token start = *peek();
    advance(); // compile_time
    advance(); // .
    advance(); // if
    if (!expect(TokenType::TOKEN_LPAREN, "'(' after 'compile_time.if'")) {
        return nullptr;
    }
    auto cond = parseExpr(2); // minPrec=2：',' 分隔分支，不作运算符吞入
    if (!cond) {
        return nullptr;
    }
    if (!expect(TokenType::TOKEN_RPAREN, "')' after 'compile_time.if' condition")) {
        return nullptr;
    }

    auto node = std::make_unique<CompileTimeIfDeclAST>();
    node->cond = std::move(cond);

    auto parseBranch = [this](std::vector<std::unique_ptr<DeclAST>>& out) -> bool {
        if (!expect(TokenType::TOKEN_LBRACE, "'{'")) {
            return false;
        }
        while (!check(TokenType::TOKEN_RBRACE) && !eof()) {
            auto decl = parseDeclaration();
            if (!decl) {
                return false;
            }
            out.push_back(std::move(decl));
        }
        return expect(TokenType::TOKEN_RBRACE, "'}'");
    };
    if (!parseBranch(node->thenDecls)) {
        return nullptr;
    }
    if (peek() && peek()->lexeme == "else") {
        advance();
        if (!parseBranch(node->elseDecls)) {
            return nullptr;
        }
    }
    node->setLocation(start.filename, start.line, start.column);
    return node;
}

std::unique_ptr<StmtAST> Parser::parseCompileTimeIfStmt() {
    Token start = *peek();
    advance(); // compile_time
    advance(); // .
    advance(); // if
    if (!expect(TokenType::TOKEN_LPAREN, "'(' after 'compile_time.if'")) return nullptr;
    auto cond = parseExpr(2);
    if (!cond) return nullptr;
    if (!expect(TokenType::TOKEN_RPAREN, "')' after 'compile_time.if' condition")) return nullptr;
    auto node = std::make_unique<CompileTimeIfStmtAST>();
    node->cond = std::move(cond);
    if (!check(TokenType::TOKEN_LBRACE)) {
        errorUnexpected("expected '{' after compile_time.if condition");
        return nullptr;
    }
    node->thenStmt = parseCompoundStmt();
    if (!node->thenStmt) return nullptr;
    if (match(TokenType::TOKEN_ELSE)) {
        if (!check(TokenType::TOKEN_LBRACE)) {
            errorUnexpected("expected '{' after 'else' in compile_time.if");
            return nullptr;
        }
        node->elseStmt = parseCompoundStmt();
        if (!node->elseStmt) return nullptr;
    }
    node->setLocation(start.filename, start.line, start.column);
    return node;
}

std::unique_ptr<StmtAST> Parser::parseCompileTimeRepeatStmt() {
    Token start = *peek();
    advance(); // compile_time
    advance(); // .
    advance(); // repeat
    if (!expect(TokenType::TOKEN_LPAREN, "'(' after 'compile_time.repeat'")) {
        return nullptr;
    }
    auto count = parseExpr(2);
    if (!count) return nullptr;
    if (!expect(TokenType::TOKEN_RPAREN, "')' after compile_time.repeat count")) {
        return nullptr;
    }
    if (!check(TokenType::TOKEN_LBRACE)) {
        errorUnexpected("expected '{' after compile_time.repeat count");
        return nullptr;
    }
    auto node = std::make_unique<CompileTimeRepeatStmtAST>();
    node->count = std::move(count);
    node->body = parseCompoundStmt();
    if (!node->body) return nullptr;
    node->setLocation(start.filename, start.line, start.column);
    return node;
}

// P1-04 / CT-01: `compile_time.static_assert(cond[, msg]);` —— 包装为
// MethodCallExprAST（object=VariableExpr("compile_time")），sema 特判求值。
std::unique_ptr<DeclAST> Parser::parseCompileTimeAssert() {
    Token start = *peek();
    advance(); // compile_time
    advance(); // .
    std::string member = peek() ? peek()->lexeme : "static_assert";
    advance(); // member
    if (!expect(TokenType::TOKEN_LPAREN, "'(' after 'compile_time." + member + "'")) {
        return nullptr;
    }
    CTArgPolicy policy = ctArgPolicyFor(member);
    std::vector<std::unique_ptr<ExprAST>> args;
    if (!check(TokenType::TOKEN_RPAREN)) {
        bool firstArg = true;
        while (true) {
            std::unique_ptr<ExprAST> arg;
            bool tryType = (policy == CTArgPolicy::AllTypes) ||
                           (policy == CTArgPolicy::FirstType && firstArg);
            if (tryType && isTypeStart()) {
                Type* ty = parseType();
                if (!ty) return nullptr;
                arg = std::make_unique<SizeofExprAST>(ty, nullptr);
            } else {
                arg = parseExpr(2); // minPrec=2：实参间 ',' 分隔（与调用实参一致）
            }
            if (!arg) {
                return nullptr;
            }
            args.push_back(std::move(arg));
            firstArg = false;
            if (match(TokenType::TOKEN_COMMA)) {
                continue;
            }
            break;
        }
    }
    if (!expect(TokenType::TOKEN_RPAREN, "')' after compile_time." + member + " arguments")) {
        return nullptr;
    }
    match(TokenType::TOKEN_SEMICOLON); // 容错：无 ';' 也不报错

    auto call = std::make_unique<MethodCallExprAST>(
        std::make_unique<VariableExprAST>("compile_time"), member, std::move(args));
    auto node = std::make_unique<CompileTimeAssertDeclAST>();
    node->call = std::move(call);
    node->setLocation(start.filename, start.line, start.column);
    return node;
}

std::unique_ptr<DeclAST> Parser::parseTemplateDecl() {
    advance(); // 消耗 'template'
    if (!expect(TokenType::TOKEN_LT, "expected '<' after 'template'")) {
        return nullptr;
    }

    m_templateScopes.emplace_back();
    auto& scope = m_templateScopes.back();
    std::vector<TemplateDeclAST::Param> params;

    auto parseParamName = [this]() -> std::string {
        if (!check(TokenType::TOKEN_IDENTIFIER)) {
            errorUnexpected("template parameter name");
            return "";
        }
        return advance()->lexeme;
    };

    while (true) {
        auto tok = peek();
        if (!tok) {
            m_templateScopes.pop_back();
            errorUnexpectedEOF("template parameter");
            return nullptr;
        }
        if (tok->type == TokenType::TOKEN_TYPENAME) {
            advance();
            std::string name = parseParamName();
            if (name.empty()) {
                m_templateScopes.pop_back();
                return nullptr;
            }
            params.push_back({name, true, nullptr});
            scope.typeParams.insert(name);
        } else if (tok->type == TokenType::TOKEN_CLASS) {
            error("use 'typename' instead of 'class' in template parameter list", *tok);
            m_templateScopes.pop_back();
            return nullptr;
        } else if (tok->type == TokenType::TOKEN_USIZE || tok->type == TokenType::TOKEN_ISIZE ||
                   tok->type == TokenType::TOKEN_INT8 || tok->type == TokenType::TOKEN_INT16 ||
                   tok->type == TokenType::TOKEN_INT32 || tok->type == TokenType::TOKEN_INT64 ||
                   tok->type == TokenType::TOKEN_UINT8 || tok->type == TokenType::TOKEN_UINT16 ||
                   tok->type == TokenType::TOKEN_UINT32 || tok->type == TokenType::TOKEN_UINT64) {
            // GEN-02: 非类型（整数）参数，如 `usize N`。
            Type* nonTypeType = parseBaseType();
            if (!nonTypeType) {
                m_templateScopes.pop_back();
                return nullptr;
            }
            std::string name = parseParamName();
            if (name.empty()) {
                m_templateScopes.pop_back();
                return nullptr;
            }
            params.push_back({name, false, nonTypeType});
            scope.valueParams.insert(name);
        } else {
            errorUnexpected("'typename' or an integer type in template parameter list");
            m_templateScopes.pop_back();
            return nullptr;
        }
        if (match(TokenType::TOKEN_COMMA)) continue;
        break;
    }

    if (!expect(TokenType::TOKEN_GT, "expected '>' to close template parameter list")) {
        m_templateScopes.pop_back();
        return nullptr;
    }

    // 解析模板体——委托给普通声明解析（函数/struct/class/using 别名共用
    // 既有路径）。委托期间 m_templateScopes 栈顶使参数名解析为占位类型。
    auto bodyStart = peek();
    auto body = parseDeclaration();
    if (!body) {
        m_templateScopes.pop_back();
        return nullptr;
    }

    if (dynamic_cast<UnionDeclAST*>(body.get()) || dynamic_cast<EnumDeclAST*>(body.get())) {
        if (bodyStart) error("'template' is not supported on unions/enums", *bodyStart);
        m_templateScopes.pop_back();
        return nullptr;
    }
    if (auto* fnBody = dynamic_cast<FunctionDeclAST*>(body.get());
        fnBody && fnBody->isCompileOnly) {
        if (bodyStart) error("compile_only is not supported on templates", *bodyStart);
        m_templateScopes.pop_back();
        return nullptr;
    }

    m_templateScopes.pop_back();

    // 模板体 parse 经普通声明路径注册的类型名占位标记为模板模式——裸模板名
    // （无实参）在类型位置由 sema 诊断（解析期占位必须保留，否则
    // isTypeStart 失灵导致声明被当表达式解析）。
    if (auto* stBody = dynamic_cast<StructDeclAST*>(body.get())) {
        if (auto* pt = TypeContext::instance().getStruct(stBody->name))
            pt->isTemplatePattern = true;
        if (auto* pc = TypeContext::instance().getClass(stBody->name))
            pc->isTemplatePattern = true;
    }

    auto tpl = std::make_unique<TemplateDeclAST>();
    tpl->params = std::move(params);
    tpl->decl = std::move(body);
    tpl->isAlias = dynamic_cast<UsingDeclAST*>(tpl->decl.get()) != nullptr;
    return tpl;
}

bool Parser::parseMemberFieldDecl(Type* baseType, MemberFieldDecl& out) {
    out = MemberFieldDecl{};
    Type* type = baseType;

    // Parenthesized declarator: `(*f)(int32)` / `(*p)[3]`.
    if (check(TokenType::TOKEN_LPAREN)) {
        std::string name;
        bool isFunction = false;
        std::vector<std::unique_ptr<ParamDeclAST>> params;
        Type* declType = parseFunctionPointerDeclarator(
            type, name, /*requireName=*/true, &isFunction, &params);
        if (!declType) return false;
        if (isFunction) {
            error("aggregate member cannot be a function", *peek());
            return false;
        }
        out.name = std::move(name);
        out.type = declType;
        return true;
    }

    // Leading pointer declarators: `*a`, `**b`, with pointer qualifiers.
    while (check(TokenType::TOKEN_STAR)) {
        advance();
        type = new Type(TypeKind::Pointer, type);
        while (check(TokenType::TOKEN_CONST) || check(TokenType::TOKEN_VOLATILE) ||
               check(TokenType::TOKEN_RESTRICT)) {
            const bool isConst = check(TokenType::TOKEN_CONST);
            const bool isVolatile = check(TokenType::TOKEN_VOLATILE);
            const bool isRestrict = check(TokenType::TOKEN_RESTRICT);
            advance();
            if (isConst) type->isConst = true;
            if (isVolatile) type->isVolatile = true;
            if (isRestrict) type->isRestrict = true;
        }
    }

    if (!check(TokenType::TOKEN_IDENTIFIER)) {
        errorUnexpected("expected member name");
        return false;
    }
    out.name = advance()->lexeme;

    if (check(TokenType::TOKEN_COLON)) {
        advance();
        int bw = 0;
        if (!parseBitFieldWidth(bw)) return false;
        if (bw == 0) {
            error("bit-field width must be non-zero for a named bit-field", *peek());
            return false;
        }
        if (check(TokenType::TOKEN_LBRACKET)) {
            error("bit-field cannot have array suffix", *peek());
            return false;
        }
        out.type = type;
        out.bitWidth = bw;
        return true;
    }

    type = parseMemberArraySuffix(type, &out.flexibleArray);
    if (!type) return false;
    out.type = type;
    return true;
}

std::unique_ptr<StructDeclAST> Parser::parseStructDecl() {
    return parseRecordDecl(/*isClass=*/false);
}

// AGG-11: register a nested type declaration into TypeContext, mirroring
// what parseDeclaration does for top-level declarations of the same kind
// (idempotent: an existing registration, e.g. from a forward declaration,
// wins).
void Parser::registerNestedDeclType(DeclAST* decl) {
    auto& tc = TypeContext::instance();
    if (auto* sd = dynamic_cast<StructDeclAST*>(decl)) {
        if (isClassLikeRecord(*sd)) {
            if (!tc.getClass(sd->name)) {
                auto* classType = new ClassType(sd->name);
                for (auto& f : sd->fields) {
                    classType->addField(f.name, f.type, f.bitWidth, f.annotations);
                    if (f.flexibleArray && !classType->fields.empty())
                        classType->fields.back().flexibleArray = true;
                }
                tc.addClass(sd->name, classType);
            }
            if (!sd->isForwardDecl) {
                if (auto* ct = tc.getClass(sd->name)) ct->parsedDefinition = true;
            }
        } else {
            StructType* structType = tc.getStruct(sd->name);
            if (!structType) {
                structType = new StructType(sd->name);
                tc.addStruct(sd->name, structType);
            }
            if (!sd->isForwardDecl) {
                structType->fields.clear();
                for (auto& f : sd->fields) {
                    structType->addField(f.name, f.type, f.bitWidth, f.annotations);
                    if (f.flexibleArray && !structType->fields.empty())
                        structType->fields.back().flexibleArray = true;
                }
                structType->parsedDefinition = true;
            }
        }
        return;
    }
    if (auto* ed = dynamic_cast<EnumDeclAST*>(decl)) {
        if (!ed->values.empty() && !tc.getEnum(ed->name)) {
            auto* enumType = new EnumType(ed->name);
            enumType->underlyingType = ed->underlyingType;
            for (auto& v : ed->values) enumType->addValue(v.first, v.second);
            tc.addEnum(ed->name, enumType);
        }
        return;
    }
    if (auto* ud = dynamic_cast<UnionDeclAST*>(decl)) {
        if (!tc.getUnion(ud->name)) {
            auto* unionType = new UnionType(ud->name);
            for (auto& m : ud->members) {
                unionType->addMember(m.name, m.type, m.bitWidth, m.annotations);
                if (m.flexibleArray && !unionType->members.empty())
                    unionType->members.back().flexibleArray = true;
            }
            tc.addUnion(ud->name, unionType);
        }
        if (!ud->isForwardDecl) {
            if (auto* ut = tc.getUnion(ud->name)) ut->parsedDefinition = true;
        }
        return;
    }
}

// AGG-11: token index just past the '}' matching the '{' at lbracePos
// (lbracePos itself when the braces are unbalanced). Used to tell a nested
// type declaration (`struct Inner { ... };`) from an inline member definition
// with a declarator (`struct Inner { ... } inner;`).
size_t Parser::balancedBraceEnd(size_t lbracePos) const {
    if (lbracePos >= m_tokens.size() ||
        m_tokens[lbracePos].type != TokenType::TOKEN_LBRACE) {
        return lbracePos;
    }
    int depth = 0;
    for (size_t i = lbracePos; i < m_tokens.size(); ++i) {
        if (m_tokens[i].type == TokenType::TOKEN_LBRACE) {
            ++depth;
        } else if (m_tokens[i].type == TokenType::TOKEN_RBRACE) {
            if (--depth == 0) return i + 1;
        }
    }
    return lbracePos;
}

bool Parser::recordBodyLooksClassLike() const {
    if (m_currentTokenPos >= m_tokens.size() ||
        m_tokens[m_currentTokenPos].type != TokenType::TOKEN_LBRACE) {
        return false;
    }
    const size_t end = balancedBraceEnd(m_currentTokenPos);
    if (end <= m_currentTokenPos) return false;

    int depth = 0;
    for (size_t i = m_currentTokenPos + 1; i < end; ++i) {
        const TokenType type = m_tokens[i].type;
        if (type == TokenType::TOKEN_LBRACE) {
            ++depth;
            continue;
        }
        if (type == TokenType::TOKEN_RBRACE) {
            if (depth > 0) --depth;
            continue;
        }
        if (depth != 0) continue;

        // Skip member annotations (`[[align(16)]]`): the identifier inside the
        // attribute is followed by '(' and would otherwise look like a method.
        if (type == TokenType::TOKEN_LBRACKET && i + 1 < end &&
            m_tokens[i + 1].type == TokenType::TOKEN_LBRACKET) {
            i += 2;
            while (i + 1 < end &&
                   !(m_tokens[i].type == TokenType::TOKEN_RBRACKET &&
                     m_tokens[i + 1].type == TokenType::TOKEN_RBRACKET)) {
                ++i;
            }
            if (i + 1 < end) ++i; // consume the closing ']]'
            continue;
        }

        if (type == TokenType::TOKEN_PUBLIC || type == TokenType::TOKEN_PRIVATE) {
            return true;
        }
        if (type == TokenType::TOKEN_IDENTIFIER && m_tokens[i].lexeme == "protected" &&
            i + 1 < end && m_tokens[i + 1].type == TokenType::TOKEN_COLON) {
            return true;
        }
        if (type == TokenType::TOKEN_OPERATOR) return true;
        // A method declaration is `type identifier '(' ...`; function-pointer
        // fields put '(' before the identifier, so they do not match.
        if (type == TokenType::TOKEN_IDENTIFIER && i + 1 < end &&
            m_tokens[i + 1].type == TokenType::TOKEN_LPAREN) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<StructDeclAST> Parser::parseClassDecl() {
    return parseRecordDecl(/*isClass=*/true);
}

std::unique_ptr<StructDeclAST> Parser::parseRecordDecl(bool isClass) {
    if (isClass) {
        if (!match(TokenType::TOKEN_CLASS)) return nullptr;
    } else {
        if (!match(TokenType::TOKEN_STRUCT)) return nullptr;
    }

    std::string bareName = parseQualifiedTypeName();
    // PAR-12: 匿名 class 定义获得唯一合成名。
    if (bareName.empty() && check(TokenType::TOKEN_LBRACE)) {
        bareName = TypeContext::instance().nextAnonId();
    }
    std::string name = qualifyTypeDeclName(bareName);

    // Parse optional inheritance: : public BaseName
    // INH-01/DS2: the specifier defaults to public (bare `class D : B` is
    // public here — deliberate deviation from C++, where it means private
    // inheritance); private/protected inheritance is diagnosed unsupported.
    std::string baseClass;
    if (match(TokenType::TOKEN_COLON)) {
        // Skip optional access specifier (public/private/protected)
        if (check(TokenType::TOKEN_PUBLIC) || check(TokenType::TOKEN_PRIVATE) || check(TokenType::TOKEN_IDENTIFIER)) {
            std::string access = peek()->lexeme;
            if (access == "public" || access == "private" || access == "protected") {
                advance();
                if (access != "public") {
                    error("private/protected inheritance is not supported; use public inheritance",
                          *peek());
                    return nullptr;
                }
            }
        }
        baseClass = qualifyTypeDeclName(parseQualifiedTypeName());
        // P1-03 / INH-05 / GEN-09: 模板实例基类 `Shape<Circle>`——实参在
        // 派生类定义处尚不存在（CRTP），parse 只捕获源拼写，sema 在派生类
        // visit 时经 Registry 解析。实参限定为类型名/数字的平拼写。
        if (check(TokenType::TOKEN_LT)) {
            std::string spelling = "<";
            advance();
            int depth = 1;
            while (!eof() && depth > 0) {
                auto t = peek();
                if (t->type == TokenType::TOKEN_GT) {
                    --depth;
                    if (depth == 0) {
                        spelling += ">";
                        advance();
                        break;
                    }
                }
                if (t->type == TokenType::TOKEN_IDENTIFIER ||
                    t->type == TokenType::TOKEN_NUMBER ||
                    t->type == TokenType::TOKEN_COMMA ||
                    t->type == TokenType::TOKEN_STAR ||
                    t->type == TokenType::TOKEN_LT ||
                    t->type == TokenType::TOKEN_GT ||
                    t->type == TokenType::TOKEN_RSHIFT) {
                    spelling += t->lexeme;
                    advance();
                } else {
                    error("unsupported template base class argument", *t);
                    return nullptr;
                }
            }
            if (depth != 0) {
                errorUnexpected("expected '>' to close template base class arguments");
                return nullptr;
            }
            baseClass += spelling;
        }
        // INH-02: single inheritance only — a ',' after the base name would
        // otherwise be silently mis-parsed as a forward declaration.
        if (check(TokenType::TOKEN_COMMA)) {
            error("multiple inheritance is not supported; use single inheritance", *peek());
            return nullptr;
        }
    }

    if (!check(TokenType::TOKEN_LBRACE)) {
        // Forward declaration
        auto decl = std::make_unique<StructDeclAST>(name, std::vector<FieldInfo>{});
        decl->baseClass = std::move(baseClass);
        decl->bareName = bareName;
        decl->isForwardDecl = true;
        decl->isClassDecl = isClass;
        return decl;
    }

    // PAR-17 / PAR-05: seed the body's self-type before parsing members.
    // Classes are always ClassType. Structs are scanned for methods/operators/
    // access sections so `S* self()`, `operator+(S)`, and `Node* next` resolve
    // in any declaration order; pure data structs stay StructType.
    if (isClass) {
        if (!TypeContext::instance().getClass(name)) {
            TypeContext::instance().addClass(name, new ClassType(name));
        }
    } else {
        const bool classLike = recordBodyLooksClassLike();
        if (classLike) {
            if (!TypeContext::instance().getClass(name)) {
                TypeContext::instance().addClass(name, new ClassType(name));
            }
            // A forward declaration may have left a StructType placeholder;
            // ClassType is the authoritative identity from here on.
            TypeContext::instance().removeStruct(name);
        } else if (!TypeContext::instance().getStruct(name)) {
            TypeContext::instance().addStruct(name, new StructType(name));
        }
    }

    advance(); // consume '{'

    // AGG-11: extend the type-name prefix with this class's bare name while
    // parsing the body (same mechanism as namespaces).
    std::string savedPrefix = m_typeNamespacePrefix;
    m_typeNamespacePrefix += mangleQualifiedTypeName(bareName) + "_";

    std::vector<FieldInfo> fields;
    std::vector<std::unique_ptr<FunctionDeclAST>> methods;
    std::vector<std::unique_ptr<VarDeclAST>> staticMembers;
    std::unordered_map<std::string, AccessLevel> memberAccess;
    // AGG-11: nested type declarations, in source order.
    std::vector<std::unique_ptr<DeclAST>> nestedTypes;
    // DEC-01: class members default to private, struct members to public;
    // `public:`/`private:`/`protected:` sections change the level for the
    // members that follow in both records.
    AccessLevel currentAccess =
        isClass ? AccessLevel::Private : AccessLevel::Public;
    bool hasAccessSections = false;
    auto accessFromLexeme = [](const std::string& s) -> AccessLevel {
        if (s == "public") return AccessLevel::Public;
        if (s == "protected") return AccessLevel::Protected;
        return AccessLevel::Private;
    };

    while (!eof() && !check(TokenType::TOKEN_RBRACE)) {
        // P1-05 / ANN-01: 字段/方法/嵌套类型/静态成员注解。
        std::vector<Annotation> memberAnns = parseAnnotations();

        // PAR-24: legacy alias forms / member-scope using are rejected explicitly.
        if (isRemovedTypeAliasStart()) {
            rejectRemovedTypeAlias();
            continue;
        }
        if (isCompileOnlyStart()) {
            Token spec = *peek();
            advance();
            error("compile_only is only valid on free functions", spec);
            skipPastSemicolon();
            continue;
        }
        if (isLocalUsingStart()) {
            auto u = parseUsingAliasDecl(/*member=*/true);
            if (!u) continue;
            u->annotations = std::move(memberAnns);
            memberAccess["type:" + u->bareName] = currentAccess;
            nestedTypes.push_back(std::move(u));
            continue;
        }

        // Access specifier section: `public:` / `private:` / `protected:`.
        // `public`/`private` are keywords; `protected` is contextual, so it
        // only counts when directly followed by ':'.
        const bool isSpecifier =
            check(TokenType::TOKEN_PUBLIC) || check(TokenType::TOKEN_PRIVATE) ||
            (check(TokenType::TOKEN_IDENTIFIER) && peek() &&
             (peek()->lexeme == "public" || peek()->lexeme == "private" ||
              peek()->lexeme == "protected") &&
             m_currentTokenPos + 1 < m_tokens.size() &&
             m_tokens[m_currentTokenPos + 1].type == TokenType::TOKEN_COLON);
        if (isSpecifier) {
            std::string lexeme = peek()->lexeme;
            currentAccess = accessFromLexeme(lexeme);
            hasAccessSections = true;
            advance(); // consume specifier
            expect(TokenType::TOKEN_COLON, "expected ':' after access specifier");
            continue;
        }

        // AGG-11: a nested type declaration is `keyword IDENT {` (a forward
        // `class D;` and an inherited `class D : Base` also route here);
        // `keyword {` stays an anonymous inline member (AGG-03) and any other
        // shape stays a field of that type. The access section in effect at
        // the declaration is recorded under `type:<name>`.
        {
            TokenType kw = peek()->type;
            if (kw == TokenType::TOKEN_ENUM || kw == TokenType::TOKEN_STRUCT ||
                kw == TokenType::TOKEN_UNION || kw == TokenType::TOKEN_CLASS) {
                size_t save = m_currentTokenPos;
                advance();
                std::string nestedName = parseQualifiedTypeName();
                // A '{' followed (after the balanced closing brace) by an
                // IDENTIFIER is an inline member definition with a declarator
                // (`struct Inner { ... } inner;`) — that stays on the field
                // path (pre-existing C-style idiom, pinned by tests).
                // A ';' is a forward declaration (`class D;`, `struct D;`).
                // `class D : Base` also routes (inherited definition or
                // forward). `enum E : under` skips the underlying-type spec
                // before the '{' / ';' check (TYP-09 form).
                bool isNested = false;
                if (!nestedName.empty()) {
                    if (check(TokenType::TOKEN_LBRACE)) {
                        size_t afterBody = balancedBraceEnd(m_currentTokenPos);
                        isNested = afterBody >= m_tokens.size() ||
                                   m_tokens[afterBody].type != TokenType::TOKEN_IDENTIFIER;
                    } else if (check(TokenType::TOKEN_SEMICOLON)) {
                        isNested = true;
                    } else if (kw == TokenType::TOKEN_CLASS &&
                               check(TokenType::TOKEN_COLON)) {
                        isNested = true;
                    } else if (kw == TokenType::TOKEN_ENUM &&
                               check(TokenType::TOKEN_COLON)) {
                        advance(); // ':'
                        if (parseType()) {
                            isNested = check(TokenType::TOKEN_LBRACE) ||
                                       check(TokenType::TOKEN_SEMICOLON);
                        }
                    }
                }
                m_currentTokenPos = save;
                if (isNested) {
                    std::unique_ptr<DeclAST> nested;
                    if (kw == TokenType::TOKEN_ENUM) {
                        nested = parseEnumDecl();
                        // parseEnumDecl stops at '}' (or right after the name
                        // for a forward) — the trailing ';' belongs to the
                        // declaration; the member loop owns it.
                        match(TokenType::TOKEN_SEMICOLON);
                    }
                    else if (kw == TokenType::TOKEN_UNION) nested = parseUnionDecl();
                    else if (kw == TokenType::TOKEN_CLASS) nested = parseClassDecl();
                    else nested = parseStructDecl();
                    // Forward declarations of struct/class/union stop before
                    // the trailing ';' — consume it here too, or it stalls the
                    // member loop and silently drops the remaining members.
                    if (kw != TokenType::TOKEN_ENUM) match(TokenType::TOKEN_SEMICOLON);
                    if (!nested) {
                        m_typeNamespacePrefix = savedPrefix;
                        return nullptr;
                    }
                    memberAccess["type:" + nestedName] = currentAccess;
                    registerNestedDeclType(nested.get());
                    if (!memberAnns.empty()) {
                        nested->annotations = std::move(memberAnns);
                    }
                    nestedTypes.push_back(std::move(nested));
                    continue;
                }
            }
        }

        // AGG-10: `static` storage for the following member (method or data).
        bool memberIsStatic = false;
        if (check(TokenType::TOKEN_STATIC)) {
            advance();
            memberIsStatic = true;
        }

        // Check if this looks like a member function declaration:
        // type identifier '(' ... or operator symbol '('
        size_t savedPos = m_currentTokenPos;

        if (isTypeStart() || check(TokenType::TOKEN_CONST) ||
            check(TokenType::TOKEN_VOLATILE) || check(TokenType::TOKEN_ATOMIC)) {
            Type* declType = parseType();
            if (!declType) break;

            // P1-07 / PAR-10: unnamed bit-field `T : N;` (incl. zero-width).
            if (check(TokenType::TOKEN_COLON)) {
                advance();
                int bw = 0;
                if (!parseBitFieldWidth(bw)) {
                    m_typeNamespacePrefix = savedPrefix;
                    return nullptr;
                }
                fields.push_back(FieldInfo{"", declType, std::move(memberAnns), bw});
                match(TokenType::TOKEN_SEMICOLON);
                continue;
            }

            auto parseFieldGroup = [&](Type* firstBase) -> bool {
                Type* memberBase = stripDeclaratorType(firstBase);
                bool firstField = true;
                while (true) {
                    MemberFieldDecl fd;
                    if (!parseMemberFieldDecl(firstField ? firstBase : memberBase, fd))
                        return false;
                    auto anns = firstField ? std::move(memberAnns)
                                           : std::vector<Annotation>{};
                    FieldInfo fi{fd.name, fd.type, std::move(anns), fd.bitWidth};
                    fi.flexibleArray = fd.flexibleArray;
                    fields.push_back(std::move(fi));
                    memberAccess[fd.name] = currentAccess;
                    firstField = false;
                    if (!match(TokenType::TOKEN_COMMA)) {
                        if (!expect(TokenType::TOKEN_SEMICOLON,
                                    "expected ';' after member declaration"))
                            return false;
                        break;
                    }
                }
                return true;
            };

            if (check(TokenType::TOKEN_IDENTIFIER)) {
                size_t namePos = m_currentTokenPos;
                std::string memberName = advance()->lexeme;

                if (check(TokenType::TOKEN_LPAREN)) {
                    // Member function declaration
                    auto func = parseFunctionDecl(declType, memberName);
                    if (func) {
                        func->annotations = std::move(memberAnns);
                        func->isStatic = memberIsStatic;
                        methods.push_back(std::move(func));
                        // I1: static methods are keyed separately so a
                        // same-name instance method's level stays intact.
                        memberAccess[memberIsStatic ? "static:" + memberName : memberName] =
                            currentAccess;
                    }
                } else if (memberIsStatic) {
                    // AGG-10: static data member — optional in-class
                    // initializer becomes the global definition (DS2);
                    // kept out of `fields` so it takes no object layout.
                    std::unique_ptr<ExprAST> init = nullptr;
                    if (match(TokenType::TOKEN_ASSIGN)) {
                        init = parseExpr(2);
                        if (!init) {
                            m_typeNamespacePrefix = savedPrefix;
                            return nullptr;
                        }
                    }
                    if (!expect(TokenType::TOKEN_SEMICOLON,
                                "expected ';' after static member declaration")) {
                        m_typeNamespacePrefix = savedPrefix;
                        return nullptr;
                    }
                    auto staticVar = std::make_unique<VarDeclAST>(memberName, declType, std::move(init));
                    staticVar->annotations = std::move(memberAnns);
                    staticMembers.push_back(std::move(staticVar));
                    // I1: access level keyed separately from instance members
                    // so same-name static/instance members stay independent.
                    memberAccess["static:" + memberName] = currentAccess;
                } else {
                    m_currentTokenPos = namePos;
                    if (!parseFieldGroup(declType)) {
                        m_typeNamespacePrefix = savedPrefix;
                        return nullptr;
                    }
                }
            } else if (check(TokenType::TOKEN_LPAREN) && !memberIsStatic) {
                // Direct function-pointer / pointer-to-array member:
                // `int32 (*f)(int32);` / `int32 (*p)[3];`.
                if (!parseFieldGroup(declType)) {
                    m_typeNamespacePrefix = savedPrefix;
                    return nullptr;
                }
            } else if (check(TokenType::TOKEN_OPERATOR)) {
                // Operator overload: type operator+(...)
                advance(); // consume 'operator'
                auto opToken = peek();
                if (opToken) {
                    std::string opSym;
                    bool symbolConsumed = false;
                    // SEM-17: `operator[]` / `operator()` — bracket pair.
                    if (opToken->type == TokenType::TOKEN_LBRACKET) {
                        advance(); // '['
                        if (!match(TokenType::TOKEN_RBRACKET)) {
                            errorUnexpected("expected ']' after 'operator['");
                            m_typeNamespacePrefix = savedPrefix;
                            return nullptr;
                        }
                        opSym = "[]";
                        symbolConsumed = true;
                    } else if (opToken->type == TokenType::TOKEN_LPAREN) {
                        advance(); // '('
                        if (!match(TokenType::TOKEN_RPAREN)) {
                            errorUnexpected("expected ')' after 'operator('");
                            m_typeNamespacePrefix = savedPrefix;
                            return nullptr;
                        }
                        opSym = "()";
                        symbolConsumed = true;
                    } else {
                        switch (opToken->type) {
                            case TokenType::TOKEN_PLUS:        opSym = "+"; break;
                            case TokenType::TOKEN_MINUS:       opSym = "-"; break;
                            case TokenType::TOKEN_STAR:        opSym = "*"; break;
                            case TokenType::TOKEN_SLASH:       opSym = "/"; break;
                            case TokenType::TOKEN_PERCENT:     opSym = "%"; break;
                            case TokenType::TOKEN_EQ:          opSym = "=="; break;
                            case TokenType::TOKEN_NOT_EQ:      opSym = "!="; break;
                            case TokenType::TOKEN_LT:          opSym = "<"; break;
                            case TokenType::TOKEN_GT:          opSym = ">"; break;
                            case TokenType::TOKEN_LE:          opSym = "<="; break;
                            case TokenType::TOKEN_GE:          opSym = ">="; break;
                            case TokenType::TOKEN_AND:         opSym = "&&"; break;
                            case TokenType::TOKEN_OR:          opSym = "||"; break;
                            case TokenType::TOKEN_BIT_AND:     opSym = "&"; break;
                            case TokenType::TOKEN_BIT_OR:      opSym = "|"; break;
                            case TokenType::TOKEN_CARET:       opSym = "^"; break;
                            case TokenType::TOKEN_LSHIFT:      opSym = "<<"; break;
                            case TokenType::TOKEN_RSHIFT:      opSym = ">>"; break;
                            case TokenType::TOKEN_TILDE:       opSym = "~"; break;
                            case TokenType::TOKEN_NOT:         opSym = "!"; break;
                            case TokenType::TOKEN_PLUS_PLUS:   opSym = "++"; break;
                            case TokenType::TOKEN_MINUS_MINUS: opSym = "--"; break;
                            default: opSym = opToken->lexeme; break;
                        }
                    }
                    std::string opName = "operator" + opSym;
                    if (!symbolConsumed) advance(); // consume operator symbol
                    auto func = parseFunctionDecl(declType, opName);
                    if (func) {
                        func->annotations = std::move(memberAnns);
                        methods.push_back(std::move(func));
                    }
                }
            } else if (isAnonymousAggregate(declType)) {
                // AGG-03: an anonymous struct/union member promotes its
                // fields/members into the enclosing aggregate. The trailing
                // ';' was already consumed by parseType's inline-definition
                // path, so nothing else to skip here.
                size_t before = fields.size();
                if (!promoteAnonymousMembers(fields, declType)) {
                    error("duplicate member name in anonymous struct/union", *peek());
                    m_typeNamespacePrefix = savedPrefix;
                    return nullptr;
                }
                for (size_t i = before; i < fields.size(); ++i) {
                    memberAccess[fields[i].name] = currentAccess;
                }
            } else {
                m_currentTokenPos = savedPos;
                break;
            }
        } else {
            break;
        }
    }

    m_typeNamespacePrefix = savedPrefix;
    match(TokenType::TOKEN_RBRACE);
    match(TokenType::TOKEN_SEMICOLON);

    auto decl = std::make_unique<StructDeclAST>(name, std::move(fields));
    decl->methods = std::move(methods);
    decl->staticMembers = std::move(staticMembers);
    decl->baseClass = std::move(baseClass);
    decl->memberAccess = std::move(memberAccess);
    decl->nestedTypes = std::move(nestedTypes);
    decl->bareName = bareName;
    decl->isClassDecl = isClass;
    decl->hasAccessSections = hasAccessSections;
    return decl;
}

Type* Parser::parseMemberArraySuffix(Type* base, bool* outFlexibleArray) {
    if (!base || !check(TokenType::TOKEN_LBRACKET)) return base;
    std::vector<int> dims;
    std::vector<bool> explicitDim;
    // P1-03 / GEN-02: 维度为模板非类型参数名（`T data[N]`）——size 记占位 0，
    // 参数名记入 ArrayType::sizeParam，实例化时由 valueArgs 具体化。
    std::vector<std::string> dimParams;
    while (check(TokenType::TOKEN_LBRACKET)) {
        advance(); // consume '['
        int size = 1;
        bool hasNumber = false;
        std::string sizeParam;
        if (!m_templateScopes.empty() && check(TokenType::TOKEN_IDENTIFIER) &&
            m_templateScopes.back().valueParams.count(peek()->lexeme)) {
            sizeParam = advance()->lexeme;
            size = 0;
            hasNumber = true;
        } else {
            if (!parseArrayDimension(size, hasNumber)) return nullptr;
        }
        expect(TokenType::TOKEN_RBRACKET, "expected ']' after array size");
        dims.push_back(size);
        explicitDim.push_back(hasNumber);
        dimParams.push_back(sizeParam);
    }
    for (size_t i = 1; i < dims.size(); ++i) {
        if (!explicitDim[i]) {
            errorUnexpected("expected array dimension: only the first dimension may be empty");
            return nullptr;
        }
    }
    // P1-07 / AGG-04: 单维空 `[]` = 柔性数组（FAM）——size 记 0 并置标记。
    // `data[3]`（显式）与 `data[][3]`（多维）保持既有行为（非 FAM）。
    if (dims.size() == 1 && !explicitDim[0]) {
        if (outFlexibleArray) *outFlexibleArray = true;
        dims[0] = 0;
    }
    // Right-to-left: `T g[2][3]` = array of 2 arrays of 3 T (C semantics).
    Type* result = base;
    for (size_t i = dims.size(); i-- > 0;) {
        result = new ArrayType(result, dims[i]);
        if (!dimParams[i].empty()) static_cast<ArrayType*>(result)->sizeParam = dimParams[i];
    }
    return result;
}

bool Parser::lookupConstValue(const std::string& name, long long& value) const {
    auto lookup = [&](const std::string& key) -> bool {
        for (auto it = m_constScopes.rbegin(); it != m_constScopes.rend(); ++it) {
            auto found = it->find(key);
            if (found != it->end()) {
                value = found->second;
                return true;
            }
        }
        return false;
    };

    if (lookup(name)) return true;
    // Enum constants may be written qualified as `E::A`.
    auto pos = name.rfind("::");
    if (pos != std::string::npos && pos + 2 < name.size()) {
        return lookup(name.substr(pos + 2));
    }
    return false;
}

void Parser::registerConstValue(const std::string& name, long long value) {
    if (m_constScopes.empty()) m_constScopes.emplace_back();
    m_constScopes.back()[name] = value;
}

bool Parser::parseArrayDimension(int& size, bool& hasSize,
                                 std::string* deferredName) {
    if (check(TokenType::TOKEN_RBRACKET)) {
        size = 0;
        hasSize = false;
        return true;
    }

    auto expr = parseExpr(2);
    if (!expr) return false;

    long long value = 0;
    auto lookup = [this](const std::string& name, long long& out) {
        return lookupConstValue(name, out);
    };
    if (!evalIntConstExpr(expr.get(), value, lookup)) {
        if (deferredName) {
            if (auto* var = dynamic_cast<VariableExprAST*>(expr.get());
                var && m_compileTimeVarNames.count(var->name)) {
                *deferredName = var->name;
                size = 0;
                hasSize = true;
                return true;
            }
        }
        errorUnexpected("variable-length arrays are not supported: array size must be a compile-time constant");
        return false;
    }
    if (value < 0 || value > INT32_MAX) {
        error("array size out of range", *peek());
        return false;
    }
    size = static_cast<int>(value);
    hasSize = true;
    return true;
}

// SG-01: parse one template argument. Type arguments use parseType; integer
// value arguments are folded with the parser's constant lookup, and a direct
// non-type parameter (`N`) is kept symbolic for instantiation.
bool Parser::parseTemplateArgument(Type*& argType, long long& argValue,
                                     std::string& argName,
                                     std::shared_ptr<ExprAST>& argExpr) {
    argType = nullptr;
    argValue = 0;
    argName.clear();
    argExpr.reset();

    if (check(TokenType::TOKEN_FLOAT)) {
        if (!m_templateArgSpeculative)
            error("non-type template argument must be an integer constant", *peek());
        return false;
    }

    bool valueExpr = false;
    if (check(TokenType::TOKEN_NUMBER) || check(TokenType::TOKEN_CHAR) ||
        check(TokenType::TOKEN_TRUE) || check(TokenType::TOKEN_FALSE) ||
        check(TokenType::TOKEN_PLUS) || check(TokenType::TOKEN_MINUS) ||
        check(TokenType::TOKEN_TILDE)) {
        valueExpr = true;
    } else if (check(TokenType::TOKEN_IDENTIFIER)) {
        // A non-type argument may be a qualified enum constant such as
        // `std::MemoryOrderAcquire`. Scan the qualified spelling before
        // deciding whether this is a type or a value; otherwise the parser
        // treats the namespace component as a type argument.
        std::string qualified = m_tokens[m_currentTokenPos].lexeme;
        size_t k = m_currentTokenPos + 1;
        while (k + 1 < m_tokens.size() &&
               m_tokens[k].type == TokenType::TOKEN_COLON_COLON &&
               m_tokens[k + 1].type == TokenType::TOKEN_IDENTIFIER) {
            qualified += "::" + m_tokens[k + 1].lexeme;
            k += 2;
        }
        long long ignored = 0;
        valueExpr = lookupConstValue(qualified, ignored) ||
                    (!m_templateScopes.empty() &&
                     m_templateScopes.back().valueParams.count(qualified));
    }

    if (valueExpr) {
        ++m_templateArgDepth;
        auto expr = parseExpr(2);
        --m_templateArgDepth;
        if (!expr) return false;
        long long value = 0;
        auto lookup = [this](const std::string& name, long long& out) {
            return lookupConstValue(name, out);
        };
        if (!evalIntConstExpr(expr.get(), value, lookup)) {
            // Keep dependent expressions (`N + 1`) for instantiation. The
            // speculative call probe may still roll back if no `(` follows
            // the closing `>`.
            argExpr = std::shared_ptr<ExprAST>(expr.release());
            return true;
        }
        argValue = value;
        return true;
    }

    argType = parseType();
    return argType != nullptr;
}

// P1-07 / PAR-10: parse + constant-evaluate a bit-field width. The caller has
// already consumed the ':'. The width must be a compile-time integer constant
// (evalIntConstExpr), matching how enum values are folded in parseEnumDecl.
bool Parser::parseBitFieldWidth(int& width) {
    auto e = parseExpr(2);
    if (!e) return false;
    long long w = 0;
    auto lookup = [this](const std::string& name, long long& out) {
        return lookupConstValue(name, out);
    };
    if (!evalIntConstExpr(e.get(), w, lookup)) {
        error("bit-field width must be a constant expression", *peek());
        return false;
    }
    // 溢出保护：static_cast<int> 前先检查 int32 范围，防止超宽值回绕成合法小值
    // 绕过 sema 的 E2026 范围检查（如 4294967297 → 1）。
    if (w > INT32_MAX || w < INT32_MIN) {
        error("bit-field width out of range", *peek());
        return false;
    }
    width = static_cast<int>(w);
    return true;
}

std::unique_ptr<UnionDeclAST> Parser::parseUnionDecl() {
    if (!match(TokenType::TOKEN_UNION)) return nullptr;

    std::string bareName = parseQualifiedTypeName();
    // PAR-12: 匿名 union 定义获得唯一合成名。
    if (bareName.empty() && check(TokenType::TOKEN_LBRACE)) {
        bareName = TypeContext::instance().nextAnonId();
    }
    std::string name = qualifyTypeDeclName(bareName);

    if (!check(TokenType::TOKEN_LBRACE)) {
        // Forward declaration
        auto fwd = std::make_unique<UnionDeclAST>(name, std::vector<FieldInfo>{});
        fwd->bareName = bareName;
        fwd->isForwardDecl = true;
        return fwd;
    }

    advance(); // consume '{'

    // AGG-11: extend the type-name prefix with this union's bare name while
    // parsing the body (same mechanism as namespaces).
    std::string savedPrefix = m_typeNamespacePrefix;
    m_typeNamespacePrefix += mangleQualifiedTypeName(bareName) + "_";

    std::vector<FieldInfo> members;
    // AGG-11: nested type declarations, in source order.
    std::vector<std::unique_ptr<DeclAST>> nestedTypes;
    while (!eof() && !check(TokenType::TOKEN_RBRACE)) {
        // P1-05 / ANN-01: union 成员注解。
        std::vector<Annotation> memberAnns = parseAnnotations();

        // PAR-24: legacy alias forms / member-scope using are rejected explicitly.
        if (isRemovedTypeAliasStart()) {
            rejectRemovedTypeAlias();
            continue;
        }
        if (isLocalUsingStart()) {
            auto u = parseUsingAliasDecl(/*member=*/true);
            if (!u) continue;
            u->annotations = std::move(memberAnns);
            nestedTypes.push_back(std::move(u));
            continue;
        }

        // AGG-11: a nested type declaration is `keyword IDENT {`; `keyword {`
        // stays an anonymous inline member (AGG-03), any other shape a field.
        {
            TokenType kw = peek()->type;
            if (kw == TokenType::TOKEN_ENUM || kw == TokenType::TOKEN_STRUCT ||
                kw == TokenType::TOKEN_UNION || kw == TokenType::TOKEN_CLASS) {
                size_t save = m_currentTokenPos;
                advance();
                std::string nestedName = parseQualifiedTypeName();
                // A '{' followed (after the balanced closing brace) by an
                // IDENTIFIER is an inline member definition with a declarator
                // (`struct Inner { ... } inner;`) — that stays on the field
                // path (pre-existing C-style idiom, pinned by tests).
                // A ';' is a forward declaration (`class D;`, `struct D;`).
                // `class D : Base` also routes (inherited definition or
                // forward). `enum E : under` skips the underlying-type spec
                // before the '{' / ';' check (TYP-09 form).
                bool isNested = false;
                if (!nestedName.empty()) {
                    if (check(TokenType::TOKEN_LBRACE)) {
                        size_t afterBody = balancedBraceEnd(m_currentTokenPos);
                        isNested = afterBody >= m_tokens.size() ||
                                   m_tokens[afterBody].type != TokenType::TOKEN_IDENTIFIER;
                    } else if (check(TokenType::TOKEN_SEMICOLON)) {
                        isNested = true;
                    } else if (kw == TokenType::TOKEN_CLASS &&
                               check(TokenType::TOKEN_COLON)) {
                        isNested = true;
                    } else if (kw == TokenType::TOKEN_ENUM &&
                               check(TokenType::TOKEN_COLON)) {
                        advance(); // ':'
                        if (parseType()) {
                            isNested = check(TokenType::TOKEN_LBRACE) ||
                                       check(TokenType::TOKEN_SEMICOLON);
                        }
                    }
                }
                m_currentTokenPos = save;
                if (isNested) {
                    std::unique_ptr<DeclAST> nested;
                    if (kw == TokenType::TOKEN_ENUM) {
                        nested = parseEnumDecl();
                        // parseEnumDecl stops at '}' (or right after the name
                        // for a forward) — the trailing ';' belongs to the
                        // declaration; the member loop owns it.
                        match(TokenType::TOKEN_SEMICOLON);
                    }
                    else if (kw == TokenType::TOKEN_UNION) nested = parseUnionDecl();
                    else if (kw == TokenType::TOKEN_CLASS) nested = parseClassDecl();
                    else nested = parseStructDecl();
                    // Forward declarations of struct/class/union stop before
                    // the trailing ';' — consume it here too, or it stalls the
                    // member loop and silently drops the remaining members.
                    if (kw != TokenType::TOKEN_ENUM) match(TokenType::TOKEN_SEMICOLON);
                    if (!nested) {
                        m_typeNamespacePrefix = savedPrefix;
                        return nullptr;
                    }
                    registerNestedDeclType(nested.get());
                    if (!memberAnns.empty()) {
                        nested->annotations = std::move(memberAnns);
                    }
                    nestedTypes.push_back(std::move(nested));
                    continue;
                }
            }
        }

        Type* memberType = parseType();
        if (!memberType) break;

        // P1-07 / PAR-10: unnamed bit-field `T : N;` (incl. zero-width).
        if (check(TokenType::TOKEN_COLON)) {
            advance();
            int bw = 0;
            if (!parseBitFieldWidth(bw)) {
                m_typeNamespacePrefix = savedPrefix;
                return nullptr;
            }
            members.push_back(FieldInfo{"", memberType, std::move(memberAnns), bw});
            match(TokenType::TOKEN_SEMICOLON);
            continue;
        }

        if (!check(TokenType::TOKEN_IDENTIFIER) && !check(TokenType::TOKEN_STAR) &&
            !check(TokenType::TOKEN_LPAREN)) {
            // AGG-03: an anonymous struct/union inside a union body promotes
            // its members; parseType already consumed the trailing ';'.
            if (isAnonymousAggregate(memberType)) {
                if (!promoteAnonymousMembers(members, memberType)) {
                    error("duplicate member name in anonymous struct/union", *peek());
                    m_typeNamespacePrefix = savedPrefix;
                    return nullptr;
                }
                continue;
            }
            break;
        }

        Type* memberBase = stripDeclaratorType(memberType);
        bool firstField = true;
        while (true) {
            MemberFieldDecl fd;
            if (!parseMemberFieldDecl(firstField ? memberType : memberBase, fd)) {
                m_typeNamespacePrefix = savedPrefix;
                return nullptr;
            }
            auto anns = firstField ? std::move(memberAnns) : std::vector<Annotation>{};
            FieldInfo fi{fd.name, fd.type, std::move(anns), fd.bitWidth};
            fi.flexibleArray = fd.flexibleArray;
            members.push_back(std::move(fi));
            firstField = false;
            if (!match(TokenType::TOKEN_COMMA)) {
                if (!expect(TokenType::TOKEN_SEMICOLON,
                            "expected ';' after member declaration")) {
                    m_typeNamespacePrefix = savedPrefix;
                    return nullptr;
                }
                break;
            }
        }
        match(TokenType::TOKEN_SEMICOLON);
    }

    m_typeNamespacePrefix = savedPrefix;
    match(TokenType::TOKEN_RBRACE);
    match(TokenType::TOKEN_SEMICOLON);

    auto decl = std::make_unique<UnionDeclAST>(name, std::move(members));
    decl->nestedTypes = std::move(nestedTypes);
    decl->bareName = bareName;
    return decl;
}

std::unique_ptr<EnumDeclAST> Parser::parseEnumDecl() {
    if (!match(TokenType::TOKEN_ENUM)) return nullptr;

    std::string bareName = parseQualifiedTypeName();
    std::string name = qualifyTypeDeclName(bareName);

    // Optional explicit underlying type: `enum E : uint8 { ... }` (TYP-09/TYP-25).
    Type* underlyingType = nullptr;
    if (check(TokenType::TOKEN_COLON)) {
        advance(); // consume ':'
        underlyingType = parseType();
        if (!underlyingType) {
            errorUnexpected("expected underlying type after ':'");
            return nullptr;
        }
    }

    // Reuse (or create and register) the enum type so that later `enum Name`
    // references share this object, including the explicit underlying type.
    EnumType* enumType = name.empty() ? nullptr : TypeContext::instance().getEnum(name);
    if (!enumType) {
        enumType = new EnumType(name);
        if (!name.empty()) {
            TypeContext::instance().addEnum(name, enumType);
        }
    }
    enumType->underlyingType = underlyingType;

    if (!check(TokenType::TOKEN_LBRACE)) {
        // Forward declaration
        auto fwd = std::make_unique<EnumDeclAST>(name,
                                                 std::vector<std::pair<std::string, int>>{},
                                                 underlyingType);
        fwd->bareName = bareName;
        fwd->isForwardDecl = true;
        return fwd;
    }

    advance(); // consume '{'

    std::vector<std::pair<std::string, int>> values;
    int currentVal = 0;

    while (!eof() && !check(TokenType::TOKEN_RBRACE)) {
        if (!check(TokenType::TOKEN_IDENTIFIER)) break;
        std::string valueName = advance()->lexeme;

        int val = currentVal;
        bool constantValue = true;
        if (check(TokenType::TOKEN_ASSIGN)) {
            advance();
            auto expr = parseExpr(2); // ',' separates enumerators
            long long evaluated = 0;
            auto lookup = [this](const std::string& name, long long& out) {
                return lookupConstValue(name, out);
            };
            if (evalIntConstExpr(expr.get(), evaluated, lookup)) {
                val = static_cast<int>(evaluated);
            } else {
                // Leave the diagnostic to sema; don't publish a bogus value to
                // later array/bit-field constant evaluation.
                constantValue = false;
            }
        }

        values.push_back({valueName, val});
        if (constantValue) registerConstValue(valueName, val);
        currentVal = val + 1;

        if (!check(TokenType::TOKEN_COMMA)) break;
        advance(); // consume ','
    }

    match(TokenType::TOKEN_RBRACE);

    // Mirror the values onto the registered type (sema repopulates from the AST).
    enumType->values.clear();
    for (auto& v : values) {
        enumType->addValue(v.first, v.second);
    }

    auto decl = std::make_unique<EnumDeclAST>(name, std::move(values), underlyingType);
    decl->bareName = bareName;
    return decl;
}

// namespace A { ... } / namespace A::B { ... } (PAR-22)
std::unique_ptr<DeclAST> Parser::parseNamespaceDecl() {
    auto startTok = peek();
    auto decl = parseNamespaceDeclImpl();
    if (decl) {
        applyLocation(decl.get(), startTok ? &*startTok : nullptr);
    }
    return decl;
}

std::unique_ptr<DeclAST> Parser::parseNamespaceDeclImpl() {
    if (!match(TokenType::TOKEN_NAMESPACE)) return nullptr;

    std::string name;
    if (check(TokenType::TOKEN_IDENTIFIER)) {
        name = advance()->lexeme;
        for (;;) {
            if (check(TokenType::TOKEN_COLON_COLON)) {
                advance();
                auto part = match(TokenType::TOKEN_IDENTIFIER);
                if (!part) break;
                name += "::" + part->lexeme;
                continue;
            }
            if (check(TokenType::TOKEN_DOT)) {
                error("namespace names use '::'; '.' is reserved for module paths",
                      *peek());
                // Consume the malformed dotted suffix up to the body so we
                // report one diagnostic and still parse the namespace body.
                while (!eof() && !check(TokenType::TOKEN_LBRACE) &&
                       !check(TokenType::TOKEN_SEMICOLON)) {
                    advance();
                }
            }
            break;
        }
    }

    if (!expect(TokenType::TOKEN_LBRACE, "expected '{' after namespace name")) {
        return nullptr;
    }

    // Qualify type declarations/references inside this namespace body.
    std::string savedPrefix = m_typeNamespacePrefix;
    if (!name.empty()) {
        m_typeNamespacePrefix += mangleQualifiedTypeName(name) + "_";
    }

    std::vector<std::unique_ptr<DeclAST>> decls;
    while (!eof() && !check(TokenType::TOKEN_RBRACE)) {
        if (check(TokenType::TOKEN_NAMESPACE)) {
            auto nested = parseNamespaceDecl();
            if (nested) decls.push_back(std::move(nested));
            else advance();
            continue;
        }
        size_t declStart = m_currentTokenPos;
        auto decl = parseDeclaration();
        if (decl) {
            decls.push_back(std::move(decl));
        } else if (m_currentTokenPos == declStart) {
            advance(); // no progress: skip one token (recover without cascading)
        }
    }
    expect(TokenType::TOKEN_RBRACE, "expected '}' to close namespace");
    match(TokenType::TOKEN_SEMICOLON);

    m_typeNamespacePrefix = savedPrefix;

    return std::make_unique<NamespaceDeclAST>(name, std::move(decls));
}
