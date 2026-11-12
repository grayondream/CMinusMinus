#include "Lexer.h"
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include "frontend/Token.h"
#include "support/Log.h"
#include "support/Utils.h"

namespace {

// LEX-17: a character is a digit of `radix` (2/8/10/16).
bool isDigitForRadix(char c, int radix) {
    switch (radix) {
        case 2:  return c == '0' || c == '1';
        case 8:  return c >= '0' && c <= '7';
        case 10: return c >= '0' && c <= '9';
        case 16: return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        default: return false;
    }
}

// Numeric value of a digit character (0 for non-digits).
unsigned digitValue(char c) {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
    return 0;
}

} // namespace

static const std::unordered_map<std::string, TokenType> keywordMap = {
    // ===== 基本类型 =====
    // `int`/`float`/`double` are no longer keywords: only fixed-width spellings
    // (`int32`, `float32`, ...) name types. `TOKEN_FLOAT` still exists as the
    // float-literal token, but the word `float` itself is an identifier.
    {"char", TokenType::TOKEN_CHAR_KW},
    {"void", TokenType::TOKEN_VOID},

    // ===== 控制流 =====
    {"if", TokenType::TOKEN_IF},
    {"else", TokenType::TOKEN_ELSE},
    {"switch", TokenType::TOKEN_SWITCH},
    {"case", TokenType::TOKEN_CASE},
    {"default", TokenType::TOKEN_DEFAULT},

    {"for", TokenType::TOKEN_FOR},
    {"while", TokenType::TOKEN_WHILE},
    {"do", TokenType::TOKEN_DO},

    {"break", TokenType::TOKEN_BREAK},
    {"continue", TokenType::TOKEN_CONTINUE},
    {"return", TokenType::TOKEN_RETURN},

    // ===== 复合类型 =====
    {"struct", TokenType::TOKEN_STRUCT},
    {"union", TokenType::TOKEN_UNION},
    {"enum", TokenType::TOKEN_ENUM},
    {"class", TokenType::TOKEN_CLASS},

    // ===== 存储类 / 修饰符 =====
    {"const", TokenType::TOKEN_CONST},
    // Review 2.6: type inference for storable closure values.
    {"auto", TokenType::TOKEN_AUTO},
    {"constexpr", TokenType::TOKEN_CONSTEXPR},
    {"static", TokenType::TOKEN_STATIC},
    {"extern", TokenType::TOKEN_EXTERN},
    {"volatile", TokenType::TOKEN_VOLATILE},

    // ===== sizeof / 类型 =====
    {"sizeof", TokenType::TOKEN_SIZEOF},
    {"typedef", TokenType::TOKEN_TYPEDEF},
    {"template", TokenType::TOKEN_TEMPLATE},
    {"typename", TokenType::TOKEN_TYPENAME},
    {"operator", TokenType::TOKEN_OPERATOR},

    // ===== C99 / 扩展（可选）=====
    {"inline", TokenType::TOKEN_INLINE},
    {"restrict", TokenType::TOKEN_RESTRICT},
    // TYP-05: char8/char16/char32 (unsigned fixed-width aliases).
    {"char8", TokenType::TOKEN_CHAR8},
    {"char16", TokenType::TOKEN_CHAR16},
    {"char32", TokenType::TOKEN_CHAR32},
    // CON-04: thread_local storage class.
    {"thread_local", TokenType::TOKEN_THREAD_LOCAL},
    // CON-01: atomic type qualifier.
    {"atomic", TokenType::TOKEN_ATOMIC},

    // ===== 布尔（如果你支持）=====
    {"bool", TokenType::TOKEN_BOOL},
    // Review 2.3: `null` is the sole null-pointer spelling. `nullptr` is no
    // longer a keyword (it resolves as an ordinary unknown identifier).

    // ===== 新增关键字 =====
    {"true", TokenType::TOKEN_TRUE},
    {"false", TokenType::TOKEN_FALSE},
    {"null", TokenType::TOKEN_NULL},
    {"namespace", TokenType::TOKEN_NAMESPACE},
    {"module", TokenType::TOKEN_MODULE},
    {"import", TokenType::TOKEN_IMPORT},
    {"export", TokenType::TOKEN_EXPORT},
    {"public", TokenType::TOKEN_PUBLIC},
    {"private", TokenType::TOKEN_PRIVATE},
    {"defer", TokenType::TOKEN_DEFER},
    {"alignof", TokenType::TOKEN_ALIGNOF},
    {"offsetof", TokenType::TOKEN_OFFSETOF},
    {"asm", TokenType::TOKEN_ASM},

    // ===== 新增整数类型 =====
    {"int8", TokenType::TOKEN_INT8},
    {"int16", TokenType::TOKEN_INT16},
    {"int32", TokenType::TOKEN_INT32},
    {"int64", TokenType::TOKEN_INT64},
    {"int128", TokenType::TOKEN_INT128},
    {"uint8", TokenType::TOKEN_UINT8},
    {"uint16", TokenType::TOKEN_UINT16},
    {"uint32", TokenType::TOKEN_UINT32},
    {"uint64", TokenType::TOKEN_UINT64},
    {"uint128", TokenType::TOKEN_UINT128},
    {"isize", TokenType::TOKEN_ISIZE},
    {"usize", TokenType::TOKEN_USIZE},

    // ===== 新增浮点类型 =====
    {"float32", TokenType::TOKEN_FLOAT32},
    {"float64", TokenType::TOKEN_FLOAT64},
    {"float16", TokenType::TOKEN_FLOAT16},
    {"float128", TokenType::TOKEN_FLOAT128},
    // TYP-04: short spellings as type keywords (lexed suffixes stay in scanNumber).
    {"f16", TokenType::TOKEN_FLOAT16},
    {"f32", TokenType::TOKEN_FLOAT32},
    {"f64", TokenType::TOKEN_FLOAT64},
    {"f128", TokenType::TOKEN_FLOAT128}
};

Lexer::Lexer(const std::string& filename, const std::string& source)
    : m_source(source), m_filename(filename) {
}

Lexer::~Lexer() {
    m_source.clear();
    m_filename.clear();
}

bool Lexer::isEof() const {
    return m_currentPos >= m_source.size();
}

char Lexer::peek() const {
    return isEof() ? '\0' : m_source[m_currentPos];
}

char Lexer::peekNext() const {
    return isEof() ? '\0' : m_source[m_currentPos + 1];
}

char Lexer::peekNext2() const {
    return (isEof() || m_currentPos + 2 >= m_source.size()) ? '\0' : m_source[m_currentPos + 2];
}

char Lexer::peekNext3() const {
    return (isEof() || m_currentPos + 3 >= m_source.size()) ? '\0' : m_source[m_currentPos + 3];
}

char Lexer::advance() {
    const char ch = peek();
    if(ch == '\n') {
        m_lineNum++;
        m_colNum = 0;
    } else {
        m_colNum++;
    }

    m_currentPos++;
    return ch;
}

char Lexer::advanceNextLine() {
    while(!isEof() && peek() != '\n') {
        advance();
    }

    return advance();
}

bool Lexer::match(const char expected) {
    if(isEof()) {
        return false;
    }

    if(peek() != expected) {
        return false;
    }

    advance();
    return true;
}
        
std::string Lexer::lexname() {
    return m_source.substr(m_startPos, m_currentPos - m_startPos);
}

Token Lexer::makeToken(const TokenType type, const std::string& lexeme, const TokenValue value) {
    auto token = Token{type, lexeme, value, m_filename, (int)m_tokenStartLine, (int)m_tokenStartCol};
    LOGI("makeToken: {}", to_string(token));
    return token;
}

void Lexer::report(DiagnosticCode code, const std::string& msg, size_t line, size_t col) {
    m_diagnostics.emplace_back(Diagnostic::Level::Error, code, msg, m_filename,
                               static_cast<int>(line), static_cast<int>(col));
}

const std::vector<Diagnostic>& Lexer::getDiagnostics() const {
    return m_diagnostics;
}
        
std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens{};
    while(!isEof()) {
        skipTrivia();
        m_startPos = m_currentPos;  
        if(isEof()) {
            break;
        }

        auto token = scanToken();
        if(token.type != TokenType::TOKEN_UNKNOWN) {
            tokens.push_back(token);
        }
    }
        
    return tokens;
}

Token Lexer::nextToken() {
    while(!isEof()) {
        skipTrivia();
        m_startPos = m_currentPos;
        if(isEof()) {
            break;
        }

        auto token = scanToken();
        if(token.type != TokenType::TOKEN_UNKNOWN) {
            return token;
        }
    }

    m_tokenStartLine = m_lineNum;
    m_tokenStartCol = m_colNum + 1;
    return makeToken(TokenType::TOKEN_EOS, "");
}

void Lexer::skipWhitespace() {
    while(true){
        const char ch = peek();
        switch(ch){
        case ' ':
        case '\t':
        case '\r':
        case '\v':
        case '\n':
            advance();
            break;
        default:
            return;
        }
    }

    return;
}
        
void Lexer::skipTrivia() {
    while (true) {
        skipWhitespace();
        const char ch = peek();
        const char nextCh = peekNext();
        if (ch == '/' && (nextCh == '/' || nextCh == '*')) {
            skipComment();
        } else {
            break;
        }
    }
}

void Lexer::skipComment() {
    while (true) {
        const char ch = peek();
        const char nextCh = peekNext();
        if (ch == '/' && nextCh == '/') {
            advanceNextLine();
        } else if (ch == '/' && nextCh == '*') {
            // LEX-13: remember where the comment opened so an unterminated
            // comment (E0002) points at its start.
            const size_t startLine = m_lineNum;
            const size_t startCol = m_colNum + 1;
            advance(); 
            advance(); 

            while (!isEof() && !(peek() == '*' && peekNext() == '/')) {
                advance();
            }

            if (isEof()) {
                report(DiagnosticCode::LexUnterminatedLiteral,
                       "unterminated block comment", startLine, startCol);
                return;
            }

            advance(); 
            advance(); 
        }
        else {
            break;
        }
    }
}

std::string Lexer::lexeme() {
    return m_source.substr(m_startPos, m_currentPos - m_startPos);
}

Token Lexer::scanIdentifier() {
    while(true){
        const char ch = peek();
        // LEX-18: UTF-8 multibyte bytes (high bit set) continue an identifier;
        // no codepoint/NFC validation (documented decision).
        if(std::isalnum(ch) or ch == '_' or (ch & 0x80) != 0) {
            advance();
        } else {
            break;
        }
    }

    const auto lexName = lexeme();
    auto it = keywordMap.find(lexName);
    if(it != keywordMap.end()) {
        return makeToken(it->second, lexName);
    }

    return makeToken(TokenType::TOKEN_IDENTIFIER, lexName);
}

void Lexer::validateDigitSeparators(const std::string& lexeme, int radix) {
    for(size_t i = 0; i < lexeme.size(); i++) {
        if(lexeme[i] != '_') continue;
        const char prev = (i > 0) ? lexeme[i - 1] : '\0';
        const char next = (i + 1 < lexeme.size()) ? lexeme[i + 1] : '\0';
        if(!isDigitForRadix(prev, radix) || !isDigitForRadix(next, radix)) {
            // Report the first misplaced separator only; keep scanning so the
            // literal (and everything after it) is still tokenized.
            report(DiagnosticCode::LexInvalidDigitSeparator,
                   "misplaced digit separator '_'",
                   m_tokenStartLine, m_tokenStartCol + i);
            return;
        }
    }
}

LiteralKind Lexer::consumeIntegerSuffix() {
    bool hasU = false;
    int lCount = 0;
    while(true) {
        const char c = peek();
        if((c == 'u' || c == 'U') && !hasU) {
            hasU = true;
            advance();
            continue;
        }
        // Long/long-long spelling: `l`, `L`, `ll`, `LL`, `lL`, `Ll`.
        if((c == 'l' || c == 'L') && lCount < 2) {
            ++lCount;
            advance();
            continue;
        }
        break;
    }
    if(hasU && lCount > 0) return LiteralKind::ULong;
    if(hasU) return LiteralKind::UInt;
    if(lCount > 0) return LiteralKind::Long;
    return LiteralKind::Int; // unsuffixed default
}

LiteralKind Lexer::consumeFloatSuffix() {
    const char c = peek();
    if(c == 'f' || c == 'F') {
        advance();
        // Maximal munch of the typed suffixes f16/f32/f64/f128.
        if(peek() == '1' && peekNext() == '6') {
            advance();
            advance();
            return LiteralKind::Float16;
        }
        if(peek() == '1' && peekNext() == '2' &&
           m_source.compare(m_currentPos, 3, "128") == 0) {
            advance();
            advance();
            advance();
            return LiteralKind::Float128;
        }
        if(peek() == '3' && peekNext() == '2') {
            advance();
            advance();
            return LiteralKind::Float32;
        }
        if(peek() == '6' && peekNext() == '4') {
            advance();
            advance();
            return LiteralKind::Float64;
        }
        return LiteralKind::Float32; // bare 'f' / 'F'
    }
    if(c == 'l' || c == 'L') {
        advance();
        return LiteralKind::Float64; // legacy long-double spelling
    }
    return LiteralKind::Float64; // unsuffixed default
}

Token Lexer::scanNumber(){
    // LEX-17: accumulate integer literals in 128 bits so overflow is detected
    // without invoking signed-overflow UB, and diagnose E0003 once when the
    // value no longer fits in uint128 (recovery: keep scanning the literal).
    bool overflowed = false;
    const unsigned __int128 maxVal = ~(unsigned __int128)0;
    auto addDigit = [&](unsigned __int128& acc, unsigned base, unsigned digit) {
        if (overflowed) return;
        if (acc > (maxVal - digit) / base) {
            overflowed = true;
            report(DiagnosticCode::LexIntegerOverflow,
                   "integer literal overflows 128 bits",
                   m_tokenStartLine, m_tokenStartCol);
        } else {
            acc = acc * base + digit;
        }
    };

    // Prefixed integer literals: binary (0b), octal (0o), hexadecimal (0x).
    // LEX-15: an optional integer suffix (u/l/ul/lu) may follow the digits.
    if(peek() == '0') {
        const char next = peekNext();
        int radix = 0;
        if(next == 'b' || next == 'B') {
            radix = 2;
        } else if(next == 'o' || next == 'O') {
            radix = 8;
        } else if(next == 'x' || next == 'X') {
            radix = 16;
        }

        if(radix != 0) {
            advance(); // 消耗 '0'
            advance(); // 消耗前缀字母
            while(isDigitForRadix(peek(), radix) || peek() == '_') {
                advance();
            }

            // SG-18: C hex floating literal `0x1.8p3` / `0x1p3` / `0x.8p1`.
            // A '.' after the hex digits or a valid binary exponent `p`
            // switches the token from integer to float; `0x1e10` stays an
            // integer because 'e' is a hex digit and there is no 'p'.
            if(radix == 16) {
                bool hasHexFraction = false;
                bool hasHexExponent = false;
                if(peek() == '.') {
                    hasHexFraction = true;
                    advance();
                    while(isDigitForRadix(peek(), 16) || peek() == '_') {
                        advance();
                    }
                }
                if(peek() == 'p' || peek() == 'P') {
                    size_t j = m_currentPos + 1;
                    if(j < m_source.size() &&
                       (m_source[j] == '+' || m_source[j] == '-')) {
                        ++j;
                    }
                    if(j < m_source.size() &&
                       (isDigitForRadix(m_source[j], 10) || m_source[j] == '_')) {
                        hasHexExponent = true;
                    }
                }
                if(hasHexExponent) {
                    advance(); // 'p' / 'P'
                    if(peek() == '+' || peek() == '-') advance();
                    while(isDigitForRadix(peek(), 10) || peek() == '_') {
                        advance();
                    }
                }
                if(hasHexFraction || hasHexExponent) {
                    const size_t numberLen = m_currentPos - m_startPos;
                    const LiteralKind floatKind = consumeFloatSuffix();
                    const auto lexName = lexeme();
                    validateDigitSeparators(lexName, 16);
                    std::string cleanStr;
                    for(size_t i = 0; i < numberLen; ++i) {
                        if(lexName[i] == '_') continue;
                        cleanStr += lexName[i];
                    }
                    double value = std::strtod(cleanStr.c_str(), nullptr);
                    Token token = makeToken(TokenType::TOKEN_FLOAT, lexName, value);
                    token.literalKind = floatKind;
                    return token;
                }
            }

            const LiteralKind kind = consumeIntegerSuffix();
            const auto lexName = lexeme();
            validateDigitSeparators(lexName, radix);

            unsigned __int128 value = 0;
            for(size_t i = 2; i < lexName.size(); i++) {
                const char c = lexName[i];
                if(c == '_') continue;
                if(!isDigitForRadix(c, radix)) break;
                addDigit(value, radix, digitValue(c));
            }
            Token token = makeToken(TokenType::TOKEN_NUMBER, lexName, value);
            token.literalKind = kind;
            return token;
        }
    }

    // 十进制整数或浮点数（允许 C 的前导小数点形式 `.5`）。
    bool hasFraction = false;
    if(peek() == '.') {
        hasFraction = true;
        advance(); // 前导小数点
        while(isDigitForRadix(peek(), 10) || peek() == '_') {
            advance();
        }
    } else {
        while(isDigitForRadix(peek(), 10) || peek() == '_') {
            advance();
        }

        // LEX-05: the grammar allows an empty fraction after the decimal point
        // (`1.` / `1.e3`), so a '.' directly following decimal digits always
        // begins the fraction part. A following '_' is consumed here too so
        // the existing LEX-17 separator validation can point at it (E0004).
        if(peek() == '.') {
            hasFraction = true;
            advance(); // 小数点
            while(isDigitForRadix(peek(), 10) || peek() == '_') {
                advance();
            }
        }
    }

    // LEX-05: exponent = ( 'e' | 'E' ) , [ '+' | '-' ] , decimal-digits.
    // Only recognised when a digit (or a misplaced '_', so E0004 can point at
    // it) follows; otherwise the 'e'/'E' is left for the next token (e.g. `1e`
    // -> NUMBER 1 + IDENT e, `1e+x` stays split), so identifiers that start
    // with 'e' keep working and no extra diagnostic code is needed.
    auto exponentFollows = [&]() -> bool {
        if(peek() != 'e' && peek() != 'E') return false;
        size_t j = m_currentPos + 1;
        if(j < m_source.size() && (m_source[j] == '+' || m_source[j] == '-')) j++;
        return j < m_source.size() &&
               (isDigitForRadix(m_source[j], 10) || m_source[j] == '_');
    };
    bool hasExponent = false;
    if(exponentFollows()) {
        hasExponent = true;
        advance(); // 'e' / 'E'
        if(peek() == '+' || peek() == '-') {
            advance();
        }
        while(isDigitForRadix(peek(), 10) || peek() == '_') {
            advance();
        }
    }

    const bool isfloat = hasFraction || hasExponent;
    if(isfloat) {
        // LEX-15: float suffix -> target type (f/F/f32 -> float32, f16/f64/f128,
        // legacy l/L -> float64; unsuffixed default is float64).
        // The numeric portion ends here; everything after it is the suffix.
        const size_t numberLen = m_currentPos - m_startPos;
        const LiteralKind kind = consumeFloatSuffix();
        const auto lexName = lexeme();
        validateDigitSeparators(lexName, 10);

        std::string cleanStr;
        for(size_t i = 0; i < numberLen; i++) {
            if(lexName[i] == '_') continue;
            cleanStr += lexName[i];
        }
        double value = 0.0;
        try {
            value = std::stod(cleanStr);
        } catch (const std::exception&) {
            // Out-of-range/negative float literal: saturate instead of throwing
            // out of the compiler (LEX-17 diagnostic is tracked separately).
            value = std::strtod(cleanStr.c_str(), nullptr);
        }
        Token token = makeToken(TokenType::TOKEN_FLOAT, lexName, value);
        token.literalKind = kind;
        return token;
    }

    // 十进制整型
    const LiteralKind kind = consumeIntegerSuffix();
    const auto lexName = lexeme();
    validateDigitSeparators(lexName, 10);

    unsigned __int128 value = 0;
    for(char c : lexName) {
        if(c == '_') continue;
        if(!isDigitForRadix(c, 10)) break;
        addDigit(value, 10, digitValue(c));
    }
    Token token = makeToken(TokenType::TOKEN_NUMBER, lexName, value);
    token.literalKind = kind;
    return token;
}

// LEX-16: multiline string `"""..."""`. Content spans lines verbatim; escape
// sequences are decoded later by the parser (shared decodeEscapes). Unterminated
// at EOF reports E0002 (same recovery as plain strings).
Token Lexer::scanMultilineString() {
    advance(); // consume first '"'
    advance(); // consume second '"'
    advance(); // consume third '"'
    while (true) {
        if (isEof()) {
            report(DiagnosticCode::LexUnterminatedLiteral,
                   "unterminated multiline string literal",
                   m_tokenStartLine, m_tokenStartCol);
            break;
        }
        if (peek() == '"' && peekNext() == '"' && peekNext2() == '"') {
            advance(); // consume closing quote
            advance();
            advance();
            break;
        }
        advance();
    }
    Token token = makeToken(TokenType::TOKEN_STRING, lexeme());
    token.isMultilineString = true;
    return token;
}

Token Lexer::scanString() {
    // 检查是否是原始字符串 r"..."
    if(peek() == 'r' && peekNext() == '"') {
        advance(); // 消耗 'r'
        advance(); // 消耗 '"'
        while(true){
            if(isEof()) {
                // LEX-13: unterminated raw string must not loop past EOF.
                report(DiagnosticCode::LexUnterminatedLiteral,
                       "unterminated raw string literal",
                       m_tokenStartLine, m_tokenStartCol);
                break;
            }
            const char ch = peek();
            if(ch == '"') {
                advance();
                break;
            } else {
                advance();
            }
        }
        return makeToken(TokenType::TOKEN_STRING, lexeme());
    }
    
    // 普通字符串
    advance();
    while(true){
        if(isEof()) {
            // LEX-13: report and stop instead of looping forever at EOF.
            report(DiagnosticCode::LexUnterminatedLiteral,
                   "unterminated string literal",
                   m_tokenStartLine, m_tokenStartCol);
            break;
        }
        const char ch = peek();
        if(ch == '"') {
            advance();
            break;
        } else if(ch == '\\') {
            // 处理转义序列
            advance(); // 消耗 '\'
            if(isEof()) {
                report(DiagnosticCode::LexUnterminatedLiteral,
                       "unterminated string literal",
                       m_tokenStartLine, m_tokenStartCol);
                break;
            }
            char escapeChar = peek();
            if(escapeChar == 'u') {
                // Unicode转义 \u{XXXX}
                advance(); // 消耗 'u'
                if(peek() == '{') {
                    advance(); // 消耗 '{'
                    while(!isEof() && peek() != '}') {
                        advance();
                    }
                    if(peek() == '}') {
                        advance(); // 消耗 '}'
                    }
                }
            } else {
                advance(); // 消耗转义字符
            }
        } else {
            advance();
        }
    }

    return makeToken(TokenType::TOKEN_STRING, lexeme());
}

// SG-18: C++ raw string `R"delim(body)delim"`. The delimiter may be empty and
// is at most 16 characters; the body is verbatim (no escape processing). The
// lexeme keeps the whole span so the parser can strip the delimiter.
Token Lexer::scanRawString() {
    advance(); // 'R'
    advance(); // '"'
    std::string delim;
    while (!isEof() && peek() != '(' && delim.size() < 16) {
        const char c = peek();
        if (c == '"' || c == '\\' ||
            std::isspace(static_cast<unsigned char>(c))) {
            report(DiagnosticCode::LexUnterminatedLiteral,
                   "invalid raw string delimiter",
                   m_tokenStartLine, m_tokenStartCol);
            break;
        }
        delim += c;
        advance();
    }
    if (peek() != '(') {
        report(DiagnosticCode::LexUnterminatedLiteral,
               "unterminated raw string literal",
               m_tokenStartLine, m_tokenStartCol);
        while (!isEof()) advance();
        return makeToken(TokenType::TOKEN_STRING, lexeme());
    }
    advance(); // '('

    const std::string terminator = ")" + delim + "\"";
    const size_t close = m_source.find(terminator, m_currentPos);
    if (close == std::string::npos) {
        report(DiagnosticCode::LexUnterminatedLiteral,
               "unterminated raw string literal",
               m_tokenStartLine, m_tokenStartCol);
        while (!isEof()) advance();
        return makeToken(TokenType::TOKEN_STRING, lexeme());
    }
    const size_t end = close + terminator.size();
    while (!isEof() && m_currentPos < end) advance();
    return makeToken(TokenType::TOKEN_STRING, lexeme());
}

// TYP-05: scan a u8/u16/u32-prefixed string. The prefix characters were
// already consumed by scanToken; this scans the quoted body exactly like a
// plain string and records the prefix on the token. lexeme() keeps the whole
// `u8"..."` span (prefix + quotes) for the parser to strip.
Token Lexer::scanPrefixedString(StringLiteralPrefix prefix) {
    advance(); // consume opening '"'
    while (true) {
        if (isEof()) {
            report(DiagnosticCode::LexUnterminatedLiteral,
                   "unterminated string literal",
                   m_tokenStartLine, m_tokenStartCol);
            break;
        }
        const char c = peek();
        if (c == '"') {
            advance();
            break;
        } else if (c == '\\') {
            advance();
            if (isEof()) {
                report(DiagnosticCode::LexUnterminatedLiteral,
                       "unterminated string literal",
                       m_tokenStartLine, m_tokenStartCol);
                break;
            }
            char escapeChar = peek();
            if (escapeChar == 'u') {
                advance();
                if (peek() == '{') {
                    advance();
                    while (!isEof() && peek() != '}') advance();
                    if (peek() == '}') advance();
                }
            } else {
                advance();
            }
        } else {
            advance();
        }
    }
    Token token = makeToken(TokenType::TOKEN_STRING, lexeme());
    token.stringPrefix = prefix;
    return token;
}

Token Lexer::scanChar() {
    advance(); // 消耗开始的引号
    while(true){
        if(isEof()) {
            // LEX-13: report and stop instead of looping forever at EOF.
            report(DiagnosticCode::LexUnterminatedLiteral,
                   "unterminated character literal",
                   m_tokenStartLine, m_tokenStartCol);
            break;
        }
        const char ch = peek();
        if(ch == '\'') {
            advance();
            break;
        } else if(ch == '\\') {
            // 处理转义序列
            advance(); // 消耗 '\'
            if(isEof()) {
                report(DiagnosticCode::LexUnterminatedLiteral,
                       "unterminated character literal",
                       m_tokenStartLine, m_tokenStartCol);
                break;
            }
            char escapeChar = peek();
            if(escapeChar == 'u') {
                // Unicode转义 \u{XXXX}
                advance(); // 消耗 'u'
                if(peek() == '{') {
                    advance(); // 消耗 '{'
                    while(!isEof() && peek() != '}') {
                        advance();
                    }
                    if(peek() == '}') {
                        advance(); // 消耗 '}'
                    }
                }
            } else {
                advance(); // 消耗转义字符
            }
        } else {
            advance();
        }
    }

    return makeToken(TokenType::TOKEN_CHAR, lexeme());
}
        
Token Lexer::scanToken() {
    // Record the token's start position before consuming any character.
    m_tokenStartLine = m_lineNum;
    m_tokenStartCol = m_colNum + 1;

    const char ch = peek();
    // SG-18: C character prefixes. Consume the prefix and let scanChar() keep
    // the whole span in the lexeme; the parser strips up to the first quote.
    if (ch == 'u' && peekNext() == '8' && peekNext2() == '\'') {
        advance();
        advance();
        return scanChar();
    }
    if ((ch == 'u' || ch == 'U' || ch == 'L') && peekNext() == '\'') {
        advance();
        return scanChar();
    }
    // SG-18: C++ raw string `R"(...)"` / `R"delim(...)delim"`.
    if (ch == 'R' && peekNext() == '"') {
        return scanRawString();
    }
    if(ch == 'r' && peekNext() == '"') {
        return scanString();
    }
    // TYP-05: u8"/u16"/u32" prefixed strings — detect before the identifier
    // branch so `u8"..."` is one STRING token, not `u8` IDENT + `"..."` STRING.
    if (ch == 'u' && peekNext() == '8' && peekNext2() == '"') {
        advance(); // 'u'
        advance(); // '8'
        return scanPrefixedString(StringLiteralPrefix::U8);
    }
    if (ch == 'u' && peekNext() == '1' && peekNext2() == '6' && peekNext3() == '"') {
        advance(); // 'u'
        advance(); // '1'
        advance(); // '6'
        return scanPrefixedString(StringLiteralPrefix::U16);
    }
    if (ch == 'u' && peekNext() == '3' && peekNext2() == '2' && peekNext3() == '"') {
        advance(); // 'u'
        advance(); // '3'
        advance(); // '2'
        return scanPrefixedString(StringLiteralPrefix::U32);
    }
    // SG-18: C++ `u"..."` / `U"..."` and C `L"..."` prefixes. The language has
    // no separate wchar_t, so L maps to the 32-bit code-unit form.
    if (ch == 'u' && peekNext() == '"') {
        advance();
        return scanPrefixedString(StringLiteralPrefix::U16);
    }
    if (ch == 'U' && peekNext() == '"') {
        advance();
        return scanPrefixedString(StringLiteralPrefix::U32);
    }
    if (ch == 'L' && peekNext() == '"') {
        advance();
        return scanPrefixedString(StringLiteralPrefix::U32);
    }
    // LEX-16: multiline string `"""..."""` — detect before the plain `"` branch.
    if(ch == '"' && peekNext() == '"' && peekNext2() == '"') {
        return scanMultilineString();
    }
    // LEX-18: a UTF-8 multibyte sequence (high bit set) may start an identifier.
    if(std::isalpha(ch) or ch == '_' or (ch & 0x80) != 0) {
        return scanIdentifier();
    }else if(std::isdigit(ch) || (ch == '.' && std::isdigit(peekNext()))) {
        return scanNumber();
    }else if(ch == '"') {
        // scanString consumes the closing quote itself; return directly so the
        // trailing advance() below does not eat the following character.
        return scanString();
    }else if(ch == '\'') {
        return scanChar();
    }

    Token token;
    switch(ch){
    case '+':
        {
            if(peekNext() == '+') {
                advance();
                token = makeToken(TokenType::TOKEN_PLUS_PLUS, lexeme());
            } else if(peekNext() == '=') {
                advance();
                token = makeToken(TokenType::TOKEN_PLUS_EQ, lexeme());
            } else {
                token = makeToken(TokenType::TOKEN_PLUS, lexeme());
            }
        } break;
    case '-':
        {
            if(peekNext() == '-') {
                advance();
                token = makeToken(TokenType::TOKEN_MINUS_MINUS, lexeme());
            } else if(peekNext() == '=') {
                advance();
                token = makeToken(TokenType::TOKEN_MINUS_EQ, lexeme());
            } else if(peekNext() == '>') {
                advance();
                token = makeToken(TokenType::TOKEN_ARROW, lexeme());
            } else {
                token = makeToken(TokenType::TOKEN_MINUS, lexeme());
            }
        } break;
    case '*':
        {
            if(peekNext() == '=') {
                advance();
                token = makeToken(TokenType::TOKEN_STAR_EQ, lexeme());
            } else {
                token = makeToken(TokenType::TOKEN_STAR, lexeme());
            }
        } break;
    case '/':
        {
            if(peekNext() == '=') {
                advance();
                token = makeToken(TokenType::TOKEN_SLASH_EQ, lexeme());
            } else {
                token = makeToken(TokenType::TOKEN_SLASH, lexeme());
            }
        } break;
    case '%':
        {
            if(peekNext() == '=') {
                advance();
                token = makeToken(TokenType::TOKEN_PERCENT_EQ, lexeme());
            } else {
                token = makeToken(TokenType::TOKEN_PERCENT, lexeme());
            }
        } break;
    case '=':
        {
            if(peekNext() == '=') {
                advance();
                token = makeToken(TokenType::TOKEN_EQ, lexeme());
            } else {
                token = makeToken(TokenType::TOKEN_ASSIGN, lexeme());
            }
        } break;
    case '!':
        {
            if(peekNext() == '=') {
                advance();
                token = makeToken(TokenType::TOKEN_NOT_EQ, lexeme());
            } else {
                token = makeToken(TokenType::TOKEN_NOT, lexeme());
            }
        } break;
    case '<':
        {
            if(peekNext() == '=') {
                advance();
                token = makeToken(TokenType::TOKEN_LE, lexeme());
            } else if(peekNext() == '<') {
                advance();
                if(peekNext() == '=') {
                    advance();
                    token = makeToken(TokenType::TOKEN_LSHIFT_EQ, lexeme());
                } else {
                    token = makeToken(TokenType::TOKEN_LSHIFT, lexeme());
                }
            } else {
                token = makeToken(TokenType::TOKEN_LT, lexeme());
            }
        } break;
    case '>':
        {
            if(peekNext() == '=') {
                advance();
                token = makeToken(TokenType::TOKEN_GE, lexeme());
            } else if(peekNext() == '>') {
                advance();
                if(peekNext() == '=') {
                    advance();
                    token = makeToken(TokenType::TOKEN_RSHIFT_EQ, lexeme());
                } else {
                    token = makeToken(TokenType::TOKEN_RSHIFT, lexeme());
                }
            } else {
                token = makeToken(TokenType::TOKEN_GT, lexeme());
            }
        } break;
    case '&':
        {
            if(peekNext() == '&') {
                advance();
                token = makeToken(TokenType::TOKEN_AND, lexeme());
            } else if(peekNext() == '=') {
                advance();
                token = makeToken(TokenType::TOKEN_AMP_EQ, lexeme());
            } else {
                token = makeToken(TokenType::TOKEN_BIT_AND, lexeme());
            }
        } break;
    case '|':
        {
            if(peekNext() == '|') {
                advance();
                token = makeToken(TokenType::TOKEN_OR, lexeme());
            } else if(peekNext() == '=') {
                advance();
                token = makeToken(TokenType::TOKEN_PIPE_EQ, lexeme());
            } else {
                token = makeToken(TokenType::TOKEN_BIT_OR, lexeme());
            }
        } break;
    case '^':
        {
            if(peekNext() == '=') {
                advance();
                token = makeToken(TokenType::TOKEN_CARET_EQ, lexeme());
            } else {
                token = makeToken(TokenType::TOKEN_CARET, lexeme());
            }
        } break;
    case '~':
        token = makeToken(TokenType::TOKEN_TILDE, lexeme());
        break;
    case '?':
        token = makeToken(TokenType::TOKEN_QUESTION, lexeme());
        break;
    case ':':
        if (peekNext() == ':') {
            advance();
            token = makeToken(TokenType::TOKEN_COLON_COLON, lexeme());
        } else {
            token = makeToken(TokenType::TOKEN_COLON, lexeme());
        }
        break;
    case '(':
        token = makeToken(TokenType::TOKEN_LPAREN, lexeme());
        break;
    case ')':
        token = makeToken(TokenType::TOKEN_RPAREN, lexeme());
        break;
    case '{':
        token = makeToken(TokenType::TOKEN_LBRACE, lexeme());
        break;
    case '}':
        token = makeToken(TokenType::TOKEN_RBRACE, lexeme());
        break;
    case '[':
        token = makeToken(TokenType::TOKEN_LBRACKET, lexeme());
        break;
    case ']':
        token = makeToken(TokenType::TOKEN_RBRACKET, lexeme());
        break;
    case ';':
        token = makeToken(TokenType::TOKEN_SEMICOLON, lexeme());
        break;
    case ',':
        token = makeToken(TokenType::TOKEN_COMMA, lexeme());
        break;
    case '.':
        if (peekNext() == '.' && m_currentPos + 2 < m_source.size() && m_source[m_currentPos + 2] == '.') {
            advance();
            advance();
            token = makeToken(TokenType::TOKEN_ELLIPSIS, lexeme());
        } else {
            token = makeToken(TokenType::TOKEN_DOT, lexeme());
        }
        break;
    case '#':
        // Legal to lex, but the parser rejects it: CDD has no
        // preprocessor (NG-02).
        token = makeToken(TokenType::TOKEN_HASH, lexeme());
        break;
    default:
        // LEX-13: unknown characters are diagnosed (E0001) and skipped so the
        // rest of the stream stays lexable; tokenize() drops the TOKEN_UNKNOWN.
        report(DiagnosticCode::LexInvalidCharacter,
               std::string("invalid character '") + ch + "'",
               m_tokenStartLine, m_tokenStartCol);
        token = makeToken(TokenType::TOKEN_UNKNOWN, std::string(1, ch));
        break;
    }

    advance();
    return token;
}


