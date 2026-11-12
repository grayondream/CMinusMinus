#pragma once

#include <string>
#include <variant>

#include <cstdint>

#include "support/LiteralKind.h"

enum class TokenType : int32_t {
    TOKEN_UNKNOWN,        // 未知的 token
    // 基本标识符与字面量
    TOKEN_IDENTIFIER,   // 变量名、函数名
    TOKEN_NUMBER,       // 数字字面量
    TOKEN_STRING,       // 字符串字面量
    TOKEN_CHAR,         // 字符字面量
  
    // 关键字
    TOKEN_FLOAT,        // 浮点字面量（`float` 不再是关键字）
    TOKEN_CHAR_KW,      // 避免和 TOKEN_CHAR 冲突
    TOKEN_VOID,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_FOR,
    TOKEN_WHILE,
    TOKEN_DO,
    TOKEN_RETURN,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_STRUCT,
    TOKEN_UNION,
    TOKEN_ENUM,
    TOKEN_CLASS,
    TOKEN_CONST,
    TOKEN_AUTO,
    TOKEN_DECLTYPE,
    TOKEN_STATIC,
    TOKEN_EXTERN,
    TOKEN_VOLATILE,
    TOKEN_ASM,
    TOKEN_SIZEOF,
    TOKEN_TYPEDEF,
    TOKEN_INLINE,
    TOKEN_RESTRICT,
    TOKEN_BOOL,
    TOKEN_SWITCH,
    TOKEN_CASE,
    TOKEN_DEFAULT,
    TOKEN_CONSTEXPR,
    TOKEN_OPERATOR,
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_NULL,
    TOKEN_NAMESPACE,
    TOKEN_MODULE,
    TOKEN_IMPORT,
    TOKEN_EXPORT,
    TOKEN_PUBLIC,
    TOKEN_PRIVATE,
    TOKEN_DEFER,
    TOKEN_ALIGNOF,
    TOKEN_OFFSETOF,
    // P1-03 / GEN-01 / PAR-21
    TOKEN_TEMPLATE,
    TOKEN_TYPENAME,
    TOKEN_INT8,
    TOKEN_INT16,
    TOKEN_INT32,
    TOKEN_INT64,
    TOKEN_INT128,
    TOKEN_UINT8,
    TOKEN_UINT16,
    TOKEN_UINT32,
    TOKEN_UINT64,
    TOKEN_UINT128,
    TOKEN_ISIZE,
    TOKEN_USIZE,
    TOKEN_FLOAT32,
    TOKEN_FLOAT64,
    TOKEN_FLOAT16,
    TOKEN_FLOAT128,
    // TYP-05: char8/char16/char32 (unsigned fixed-width aliases).
    TOKEN_CHAR8,
    TOKEN_CHAR16,
    TOKEN_CHAR32,
    // CON-04: thread_local storage class.
    TOKEN_THREAD_LOCAL,
    // CON-01: atomic type qualifier.
    TOKEN_ATOMIC,

    // 运算符
    TOKEN_PLUS,         // +
    TOKEN_MINUS,        // -
    TOKEN_STAR,         // *
    TOKEN_SLASH,        // /
    TOKEN_PERCENT,      // %
    TOKEN_EQ,           // ==
    TOKEN_NOT_EQ,       // !=
    TOKEN_ASSIGN,       // =
    TOKEN_PLUS_EQ,      // +=
    TOKEN_MINUS_EQ,     // -=
    TOKEN_STAR_EQ,      // *=
    TOKEN_SLASH_EQ,     // /=
    TOKEN_LT,           // <
    TOKEN_GT,           // >
    TOKEN_LE,           // <=
    TOKEN_GE,           // >=
    TOKEN_AND,          // &&
    TOKEN_OR,           // ||
    TOKEN_NOT,          // !
    TOKEN_BIT_AND,      // &
    TOKEN_BIT_OR,       // |
    TOKEN_CARET,        // ^
    TOKEN_TILDE,        // ~
    TOKEN_LSHIFT,       // <<
    TOKEN_RSHIFT,       // >>
    TOKEN_PLUS_PLUS,    // ++
    TOKEN_MINUS_MINUS,  // --
    TOKEN_QUESTION,     // ?
    TOKEN_COLON,        // :
    TOKEN_COLON_COLON,  // ::
    TOKEN_AMP_EQ,       // &=
    TOKEN_PIPE_EQ,      // |=
    TOKEN_CARET_EQ,     // ^=
    TOKEN_LSHIFT_EQ,    // <<=
    TOKEN_RSHIFT_EQ,    // >>=
    TOKEN_PERCENT_EQ,   // %=

    // 分隔符
    TOKEN_SEMICOLON,    // ;
    TOKEN_COMMA,        // ,
    TOKEN_DOT,          // .
    TOKEN_ARROW,        // ->
    TOKEN_LPAREN,       // (
    TOKEN_RPAREN,       // )
    TOKEN_LBRACE,       // {
    TOKEN_RBRACE,       // }
    TOKEN_LBRACKET,     // [
    TOKEN_RBRACKET,     // ]
    TOKEN_ELLIPSIS,     // ...
    TOKEN_HASH,         // '#' — recognized only to reject C preprocessor directives

    // 文件结尾
    TOKEN_EOS
};

// 整型字面量统一以 unsigned __int128 承载（LEX-17：累加/溢出到 128 位；
// 消费端按需拆 low/high 或截断）。≤64 位值 hi 恒 0，行为与旧 long long 一致。
using TokenValue = std::variant<std::monostate, unsigned __int128, double, char, std::string>;
class Token {
public:
    TokenType type{}; 
    std::string lexeme{};      // 源文本
    TokenValue value{};        // 解析后的字面量
    std::string filename{};    // 文件名
    int line{};                // 行号
    int column{};              // 列号
    LiteralKind literalKind{LiteralKind::None}; // LEX-15: numeric literal kind
    // TYP-05: u8/u16/u32 string-literal prefix (None = plain `"..."`).
    StringLiteralPrefix stringPrefix{StringLiteralPrefix::None};
    // LEX-16: multiline string `"""..."""` — lexeme keeps all three quotes per
    // side; the parser strips three when this flag is set (vs one for a
    // regular string).
    bool isMultilineString{};

    Token() = default;
    Token(TokenType type,
          const std::string& lexeme,
          TokenValue value = std::monostate{},
          const std::string& filename = "",
          int line = 0,
          int column = 0)
        : type(type), lexeme(lexeme), value(value),
          filename(filename), line(line), column(column) {}
};

