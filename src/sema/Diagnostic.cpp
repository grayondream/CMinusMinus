#include "sema/Diagnostic.h"

namespace {

const DiagnosticInfo kUnknown{DiagnosticCode::None, "", "unknown diagnostic"};

const DiagnosticInfo kRegistry[] = {
    {DiagnosticCode::LexInvalidCharacter,       "E0001", "invalid character"},
    {DiagnosticCode::LexUnterminatedLiteral,     "E0002", "unterminated literal"},
    {DiagnosticCode::LexIntegerOverflow,         "E0003", "integer literal overflow"},
    {DiagnosticCode::LexInvalidDigitSeparator,   "E0004", "misplaced digit separator"},

    {DiagnosticCode::SynUnexpectedToken,         "E1001", "unexpected token"},
    {DiagnosticCode::SynExpected,                "E1002", "expected token"},

    {DiagnosticCode::SemUndeclaredIdentifier,    "E2001", "undeclared identifier"},
    {DiagnosticCode::SemTypeMismatch,            "E2002", "type mismatch"},
    {DiagnosticCode::SemIncompatibleAssignment,  "E2003", "incompatible assignment"},
    {DiagnosticCode::SemRedefinition,            "E2004", "redefinition"},
    {DiagnosticCode::SemInvalidOperand,          "E2005", "invalid operand"},
    {DiagnosticCode::SemIncompatibleCast,        "E2006", "incompatible cast"},
    {DiagnosticCode::SemUnresolvedCall,          "E2007", "unresolved call"},
    {DiagnosticCode::SemAmbiguousCall,           "E2008", "ambiguous call"},
    {DiagnosticCode::SemPrivateMemberAccess,     "E2009", "private member access"},
    {DiagnosticCode::SemUnknownAnnotation,        "E2010", "unknown annotation"},
    {DiagnosticCode::SemInvalidAnnotationTarget,  "E2011", "annotation target mismatch"},
    {DiagnosticCode::SemDuplicateAnnotation,      "E2012", "duplicate annotation"},
    {DiagnosticCode::SemAlignNotPowerOfTwo,       "E2013", "align must be a power of two"},
    {DiagnosticCode::SemAnnotationArgNotConstant, "E2014", "annotation argument not constant"},
    {DiagnosticCode::SemInvalidUtf8,              "E2015", "invalid UTF-8 sequence"},
    {DiagnosticCode::SemFormatArgCount,           "E2020", "format argument count mismatch"},
    {DiagnosticCode::SemFormatSpecType,           "E2021", "format spec does not match argument type"},
    {DiagnosticCode::SemFormatSpecSyntax,         "E2022", "invalid format spec"},
    {DiagnosticCode::SemAsmOperand,               "E2023", "invalid asm operand or constraint"},
    {DiagnosticCode::SemAsmTemplate,              "E2024", "invalid asm template"},
    // P1-07 / SEM-12
    {DiagnosticCode::SemBitFieldInvalidType,      "E2025", "invalid bit-field base type"},
    {DiagnosticCode::SemBitFieldWidthOutOfRange,  "E2026", "bit-field width out of range"},
    {DiagnosticCode::SemBitFieldNotAddressable,   "E2027", "bit-field is not addressable"},
    {DiagnosticCode::SemInvalidThreadLocalOnFunction, "E2028", "thread_local cannot be applied to a function"},
    {DiagnosticCode::SemAtomicNonScalar, "E2029", "atomic requires an integer scalar type"},
    {DiagnosticCode::SemAtomicBadRMWOp, "E2030", "operator not supported on atomic value (need CAS)"},
    {DiagnosticCode::SemUnknownType, "E2031", "unknown type"},
    {DiagnosticCode::SemReflectNonAggregate, "E2032", "fields_of requires an aggregate type"},

    {DiagnosticCode::WarnUninitializedVariable,  "W3001", "variable may be uninitialized"},
    {DiagnosticCode::WarnUnusedVariable,         "W3002", "unused variable"},
    {DiagnosticCode::WarnUnreachableCode,        "W3003", "unreachable code"},
    {DiagnosticCode::WarnDeprecated,             "W3004", "deprecated API"},
    {DiagnosticCode::WarnNullNonnull,            "W3005", "null passed to nonnull parameter"},
    {DiagnosticCode::WarnClosureEscape,           "W3006", "closure captures storage that may not outlive it"},
};

} // namespace

const DiagnosticInfo& diagnosticInfo(DiagnosticCode code) {
    for (const auto& info : kRegistry) {
        if (info.code == code) {
            return info;
        }
    }
    return kUnknown;
}

std::string diagnosticId(DiagnosticCode code) {
    return diagnosticInfo(code).id;
}
