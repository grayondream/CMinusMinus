# Class Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add class/struct support with member functions, `this` pointer, and public inheritance to Cmm compiler.

**Architecture:** Extend existing `StructDeclAST` to hold methods and base class info. Parser recognizes `class` keyword same as `struct`. Semantic analyzer validates inheritance and method resolution. Codegen generates methods as separate functions with `this` as first parameter. Inheritance via struct embedding.

**Tech Stack:** C++20, LLVM 18.1.6, Google Test

## Global Constraints

- LLVM 18.1.6 via vcpkg
- C++20 standard
- Google Test for testing
- x86_64 Linux platform
- Debug build with coverage
- All existing 415 tests must continue passing

---

## File Structure

### New Files
- `tests/ast/test_class_support.cpp` — Unit tests for class-related AST functionality

### Modified Files
- `src/frontend/Token.h` — Add `TOKEN_CLASS` token type
- `src/frontend/Lexer.cpp` — Map `"class"` keyword to `TOKEN_CLASS`
- `src/frontend/Parser.h` — Add `parseClassDecl()` method declaration
- `src/frontend/Parser.cpp` — Implement `parseClassDecl()`, handle inheritance, parse member functions
- `src/ast/Decl.h` — Add `methods` and `baseClass` fields to `StructDeclAST`
- `src/ast/Decl.cpp` — Update `StructDeclAST::codegen()` to handle methods and inheritance
- `src/ast/Symbol.h` — Add `ClassType` to type system
- `src/ast/Symbol.cpp` — Implement `ClassType` if needed
- `src/ast/Mangle.cpp` — Extend mangling for class methods
- `src/sema/SemanticAnalyzer.h` — Add class-related visit methods
- `src/sema/SemanticAnalyzer.cpp` — Implement method resolution, `this` injection, inheritance validation
- `src/codegen/CodegenContext.h` — Add `getClassType()` method if needed
- `src/codegen/CodegenContext.cpp` — Handle `TypeKind::Class` in `getLLVMType()`

---

## Tasks

### Task 1: Add `class` Keyword to Lexer

**Files:**
- Modify: `src/frontend/Token.h:1` (add `TOKEN_CLASS` to enum)
- Modify: `src/frontend/Lexer.cpp:32` (add keyword mapping)

**Interfaces:**
- Produces: `TOKEN_CLASS` token type for parser

- [ ] **Step 1: Add `TOKEN_CLASS` to TokenType enum**

```cpp
// src/frontend/Token.h
enum class TokenType {
    // ... existing tokens ...
    TOKEN_CLASS,
    // ... rest of tokens ...
};
```

- [ ] **Step 2: Map `"class"` keyword in Lexer**

```cpp
// src/frontend/Lexer.cpp, in keyword mapping section
{"class", TokenType::TOKEN_CLASS},
```

- [ ] **Step 3: Write unit test for class keyword parsing**

```cpp
// tests/ast/test_class_support.cpp
#include <gtest/gtest.h>
#include "frontend/Lexer.h"
#include "frontend/Parser.h"

TEST(ClassSupport, ClassKeywordParses) {
    std::string source = "class Foo { int x; };";
    Lexer lexer("test.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    EXPECT_EQ(ast->getDecls().size(), 1);
}
```

- [ ] **Step 4: Run test to verify it fails**

Run: `cd build && ctest -R ClassSupport --output-on-failure`
Expected: FAIL (parse error, `class` not recognized)

- [ ] **Step 5: Run test to verify it passes**

Run: `cd build && cmake --build . && ctest -R ClassSupport --output-on-failure`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/frontend/Token.h src/frontend/Lexer.cpp tests/ast/test_class_support.cpp
git commit -m "feat: add class keyword to lexer"
```

---

### Task 2: Parse Class Declarations

**Files:**
- Modify: `src/frontend/Parser.h:1` (add `parseClassDecl()` declaration)
- Modify: `src/frontend/Parser.cpp:1451-1482` (implement `parseClassDecl()`)
- Modify: `src/ast/Decl.h:84-92` (add `methods` and `baseClass` to `StructDeclAST`)

**Interfaces:**
- Consumes: `TOKEN_CLASS` token from Task 1
- Produces: `StructDeclAST` with `methods` and `baseClass` fields

- [ ] **Step 1: Add `methods` and `baseClass` fields to `StructDeclAST`**

```cpp
// src/ast/Decl.h
struct StructDeclAST : DeclAST {
    std::string name;
    std::vector<std::pair<std::string, Type*>> fields;
    std::vector<FunctionDeclAST*> methods;  // NEW
    std::string baseClass;                  // NEW (empty = no inheritance)
    // ... rest of struct ...
};
```

- [ ] **Step 2: Add `parseClassDecl()` method declaration**

```cpp
// src/frontend/Parser.h
class Parser {
    // ... existing methods ...
    std::unique_ptr<StructDeclAST> parseClassDecl();
    // ... rest of methods ...
};
```

- [ ] **Step 3: Implement `parseClassDecl()`**

```cpp
// src/frontend/Parser.cpp
std::unique_ptr<StructDeclAST> Parser::parseClassDecl() {
    consume(); // consume 'class'
    if (currentToken().type != TokenType::TOKEN_IDENTIFIER)
        return nullptr;
    
    std::string name = currentToken().value;
    consume();
    
    std::string baseClass;
    if (currentToken().type == TokenType::TOKEN_COLON) {
        consume(); // consume ':'
        if (currentToken().type == TokenType::TOKEN_PUBLIC)
            consume(); // consume 'public'
        if (currentToken().type == TokenType::TOKEN_IDENTIFIER) {
            baseClass = currentToken().value;
            consume();
        }
    }
    
    if (currentToken().type != TokenType::TOKEN_LBRACE)
        return nullptr;
    consume(); // consume '{'
    
    auto classDecl = std::make_unique<StructDeclAST>(name);
    classDecl->baseClass = baseClass;
    
    while (currentToken().type != TokenType::TOKEN_RBRACE) {
        if (currentToken().type == TokenType::TOKEN_SEMICOLON) {
            consume();
            continue;
        }
        
        // Parse member function
        if (isTypeStart() || currentToken().type == TokenType::TOKEN_VOID) {
            auto func = parseFunctionDecl();
            if (func) {
                classDecl->methods.push_back(func.release());
            }
        }
        // Parse field
        else if (currentToken().type == TokenType::TOKEN_IDENTIFIER) {
            auto type = parseBaseType();
            if (type && currentToken().type == TokenType::TOKEN_IDENTIFIER) {
                std::string fieldName = currentToken().value;
                consume();
                classDecl->fields.push_back({fieldName, type});
            }
        }
        
        if (currentToken().type == TokenType::TOKEN_SEMICOLON)
            consume();
    }
    
    consume(); // consume '}'
    if (currentToken().type == TokenType::TOKEN_SEMICOLON)
        consume(); // consume optional ';'
    
    return classDecl;
}
```

- [ ] **Step 4: Update parser to call `parseClassDecl()` for `TOKEN_CLASS`**

```cpp
// src/frontend/Parser.cpp, in parseDeclaration()
if (currentToken().type == TokenType::TOKEN_CLASS) {
    return parseClassDecl();
}
```

- [ ] **Step 5: Write unit test for class parsing**

```cpp
// tests/ast/test_class_support.cpp
TEST(ClassSupport, ClassWithMethods) {
    std::string source = R"(
        class Foo {
            int x;
            void setX(int v) { this->x = v; }
            int getX() { return this->x; }
        };
    )";
    Lexer lexer("test.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    ASSERT_EQ(ast->getDecls().size(), 1);
    
    auto& classDecl = dynamic_cast<StructDeclAST&>(*ast->getDecls()[0]);
    EXPECT_EQ(classDecl.name, "Foo");
    EXPECT_EQ(classDecl.fields.size(), 1);
    EXPECT_EQ(classDecl.methods.size(), 2);
}

TEST(ClassSupport, ClassInheritance) {
    std::string source = R"(
        class Base { int x; };
        class Derived : public Base { int y; };
    )";
    Lexer lexer("test.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    ASSERT_EQ(ast->getDecls().size(), 2);
    
    auto& derivedDecl = dynamic_cast<StructDeclAST&>(*ast->getDecls()[1]);
    EXPECT_EQ(derivedDecl.name, "Derived");
    EXPECT_EQ(derivedDecl.baseClass, "Base");
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cd build && cmake --build . && ctest -R ClassSupport --output-on-failure`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add src/frontend/Parser.h src/frontend/Parser.cpp src/ast/Decl.h tests/ast/test_class_support.cpp
git commit -m "feat: add class declaration parsing with methods and inheritance"
```

---

### Task 3: Add Class Type to Type System

**Files:**
- Modify: `src/ast/Symbol.h:14-15` (add `TypeKind::Class` to enum)
- Modify: `src/ast/Symbol.h:42-66` (add `ClassType` class)
- Modify: `src/ast/Symbol.cpp:1` (implement `ClassType` if needed)
- Modify: `src/ast/Mangle.cpp:1` (extend mangling for class methods)

**Interfaces:**
- Consumes: `StructDeclAST` with methods from Task 2
- Produces: `ClassType` for type system

- [ ] **Step 1: Add `TypeKind::Class` to enum**

```cpp
// src/ast/Symbol.h
enum class TypeKind {
    // ... existing kinds ...
    Class,
    // ... rest of kinds ...
};
```

- [ ] **Step 2: Add `ClassType` class**

```cpp
// src/ast/Symbol.h
struct ClassType : Type {
    std::string name;
    std::vector<std::pair<std::string, Type*>> fields;
    std::vector<FunctionDeclAST*> methods;
    std::string baseClass;
    
    ClassType(const std::string& name) 
        : Type(TypeKind::Class), name(name) {}
    
    void addField(const std::string& fieldName, Type* fieldType) {
        fields.push_back({fieldName, fieldType});
    }
    
    Type* getFieldType(const std::string& fieldName) const {
        for (auto& [name, type] : fields) {
            if (name == fieldName) return type;
        }
        return nullptr;
    }
    
    FunctionDeclAST* getMethod(const std::string& methodName) const {
        for (auto* method : methods) {
            if (method->name == methodName) return method;
        }
        return nullptr;
    }
};
```

- [ ] **Step 3: Extend `TypeContext` to handle classes**

```cpp
// src/ast/Symbol.h
class TypeContext {
    // ... existing fields ...
    std::unordered_map<std::string, ClassType*> m_classes;
    
public:
    // ... existing methods ...
    ClassType* getOrCreateClass(const std::string& name) {
        if (m_classes.find(name) == m_classes.end()) {
            m_classes[name] = new ClassType(name);
        }
        return m_classes[name];
    }
    
    ClassType* getClass(const std::string& name) const {
        auto it = m_classes.find(name);
        return it != m_classes.end() ? it->second : nullptr;
    }
};
```

- [ ] **Step 4: Extend name mangling for class methods**

```cpp
// src/ast/Mangle.cpp
std::string mangleFunction(const std::string& className, 
                           const std::string& methodName,
                           const std::vector<Type*>& paramTypes) {
    std::string mangled = className + "_" + methodName;
    for (auto* type : paramTypes) {
        mangled += "_" + typeToMangled(type);
    }
    return mangled;
}
```

- [ ] **Step 5: Write unit test for ClassType**

```cpp
// tests/ast/test_class_support.cpp
TEST(ClassSupport, ClassTypeCreation) {
    TypeContext ctx;
    auto* classType = ctx.getOrCreateClass("Foo");
    EXPECT_NE(classType, nullptr);
    EXPECT_EQ(classType->name, "Foo");
    EXPECT_EQ(classType->fields.size(), 0);
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cd build && cmake --build . && ctest -R ClassSupport --output-on-failure`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add src/ast/Symbol.h src/ast/Symbol.cpp src/ast/Mangle.cpp tests/ast/test_class_support.cpp
git commit -m "feat: add ClassType to type system"
```

---

### Task 4: Semantic Analysis for Classes

**Files:**
- Modify: `src/sema/SemanticAnalyzer.h:1` (add `visit(StructDeclAST&)` update)
- Modify: `src/sema/SemanticAnalyzer.cpp:823-829` (implement class-specific semantic analysis)

**Interfaces:**
- Consumes: `StructDeclAST` with methods and base class from Task 2
- Produces: Validates inheritance, injects `this` pointer, resolves methods

- [ ] **Step 1: Update `visit(StructDeclAST&)` to handle classes**

```cpp
// src/sema/SemanticAnalyzer.cpp
void SemanticAnalyzer::visit(StructDeclAST& node) {
    if (node.methods.empty() && node.baseClass.empty()) {
        // Regular struct
        auto* structType = m_typeContext.getOrCreateStruct(node.name);
        for (auto& [fieldName, fieldType] : node.fields) {
            structType->addField(fieldName, fieldType);
        }
        m_currentScope->define(node.name, structType);
    } else {
        // Class with methods or inheritance
        auto* classType = m_typeContext.getOrCreateClass(node.name);
        for (auto& [fieldName, fieldType] : node.fields) {
            classType->addField(fieldName, fieldType);
        }
        classType->methods = node.methods;
        classType->baseClass = node.baseClass;
        
        // Validate inheritance
        if (!node.baseClass.empty()) {
            auto* baseType = m_typeContext.getClass(node.baseClass);
            if (!baseType) {
                reportError("base class '" + node.baseClass + "' not found");
            }
        }
        
        m_currentScope->define(node.name, classType);
    }
}
```

- [ ] **Step 2: Add `this` pointer injection for method calls**

```cpp
// src/sema/SemanticAnalyzer.cpp
void SemanticAnalyzer::visit(CallExprAST& node) {
    // ... existing call handling ...
    
    // Check if this is a method call (e.g., obj.method())
    if (auto* memberAccess = dynamic_cast<MemberAccessExprAST*>(node.callee.get())) {
        // ... method call handling ...
        // Inject 'this' pointer as first argument
    }
}
```

- [ ] **Step 3: Add method resolution**

```cpp
// src/sema/SemanticAnalyzer.cpp
FunctionDeclAST* SemanticAnalyzer::resolveMethod(ClassType* classType, 
                                                 const std::string& methodName) {
    // Search derived class first
    if (auto* method = classType->getMethod(methodName)) {
        return method;
    }
    
    // Search base classes
    if (!classType->baseClass.empty()) {
        auto* baseType = m_typeContext.getClass(classType->baseClass);
        if (baseType) {
            return resolveMethod(baseType, methodName);
        }
    }
    
    return nullptr;
}
```

- [ ] **Step 4: Write unit test for semantic analysis**

```cpp
// tests/ast/test_class_support.cpp
TEST(ClassSupport, SemanticAnalysisValidClass) {
    std::string source = R"(
        class Foo {
            int x;
            void setX(int v) { this->x = v; }
        };
    )";
    Lexer lexer("test.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd build && cmake --build . && ctest -R ClassSupport --output-on-failure`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/sema/SemanticAnalyzer.h src/sema/SemanticAnalyzer.cpp tests/ast/test_class_support.cpp
git commit -m "feat: add semantic analysis for classes"
```

---

### Task 5: Code Generation for Classes

**Files:**
- Modify: `src/ast/Decl.cpp:146-158` (update `StructDeclAST::codegen()`)
- Modify: `src/codegen/CodegenContext.cpp:191-226` (handle `TypeKind::Class` in `getLLVMType()`)

**Interfaces:**
- Consumes: `StructDeclAST` with methods and base class from Task 4
- Produces: LLVM IR with methods as separate functions, `this` as first parameter

- [ ] **Step 1: Update `StructDeclAST::codegen()` to handle methods**

```cpp
// src/ast/Decl.cpp
void StructDeclAST::codegen(CodegenContext& ctx) {
    if (methods.empty() && baseClass.empty()) {
        // Regular struct codegen
        // ... existing code ...
    } else {
        // Class codegen
        // Create LLVM struct type
        std::vector<llvm::Type*> fieldTypes;
        for (auto& [fieldName, fieldType] : fields) {
            fieldTypes.push_back(ctx.getLLVMType(fieldType));
        }
        
        // If has base class, add it as first field
        if (!baseClass.empty()) {
            auto* baseType = ctx.getLLVMType(baseClass);
            fieldTypes.insert(fieldTypes.begin(), baseType);
        }
        
        llvm::StructType* classType = llvm::StructType::create(
            ctx.getContext(), fieldTypes, name);
        
        // Generate methods
        for (auto* method : methods) {
            method->codegen(ctx);
        }
    }
}
```

- [ ] **Step 2: Handle `TypeKind::Class` in `getLLVMType()`**

```cpp
// src/codegen/CodegenContext.cpp
case TypeKind::Class: {
    auto* classType = dynamic_cast<ClassType*>(type);
    return llvm::StructType::getTypeByName(getContext(), classType->name);
}
```

- [ ] **Step 3: Write E2E test for class codegen**

```cpp
// tests/ast/test_class_support.cpp
TEST(ClassSupport, ClassCodegen) {
    std::string source = R"(
        class Foo {
            int x;
            void setX(int v) { this->x = v; }
            int getX() { return this->x; }
        };
        
        int main() {
            Foo f;
            f.setX(42);
            return f.getX();
        }
    )";
    // ... test codegen produces valid LLVM IR ...
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd build && cmake --build . && ctest -R ClassSupport --output-on-failure`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/ast/Decl.cpp src/codegen/CodegenContext.cpp tests/ast/test_class_support.cpp
git commit -m "feat: add code generation for classes"
```

---

### Task 6: E2E Tests for Class Features

**Files:**
- Create: `tests/e2e/test_class_support.cpp` (comprehensive E2E tests)

**Interfaces:**
- Consumes: Full class support from Tasks 1-5
- Produces: Validates end-to-end class functionality

- [ ] **Step 1: Create basic class E2E test**

```cpp
// tests/e2e/test_class_support.cpp
#include <gtest/gtest.h>
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "codegen/CodegenContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"

class ClassE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        llvm::InitializeAllTargetInfos();
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmParsers();
        llvm::InitializeAllAsmPrinters();
    }
    
    int runSource(const std::string& source, const std::string& filename) {
        Lexer lexer(filename, source);
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        auto ast = parser.parse();
        if (!ast) return -1;

        SemanticAnalyzer analyzer;
        analyzer.analyze(*ast);
        if (!analyzer.getErrors().empty()) return -1;

        CodegenContext ctx;
        ctx.setSourceFile(filename);
        ast->codegen(ctx);
        ctx.finalizeDebugInfo();

        std::string ve;
        llvm::raw_string_ostream vs(ve);
        bool bad = llvm::verifyModule(ctx.getModule(), &vs);
        if (bad) return -1;

        auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
        if (!jtmb) return -1;
        auto jit = llvm::orc::LLJITBuilder()
            .setJITTargetMachineBuilder(std::move(*jtmb))
            .create();
        if (!jit) return -1;

        auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
        if (auto e = (*jit)->addIRModule(std::move(ts))) return -1;

        auto sym = (*jit)->lookup("main");
        if (!sym) return -1;

        auto fn = (int (*)())(intptr_t)sym->getValue();
        return fn();
    }
};

TEST_F(ClassE2ETest, BasicClass) {
    EXPECT_EQ(runSource(R"(
        class Foo {
            int x;
            void setX(int v) { this->x = v; }
            int getX() { return this->x; }
        };
        
        int main() {
            Foo f;
            f.setX(42);
            return f.getX();
        }
    )", "test_class.c"), 42);
}

TEST_F(ClassE2ETest, ClassInheritance) {
    EXPECT_EQ(runSource(R"(
        class Base {
            int x;
            void setX(int v) { this->x = v; }
        };
        
        class Derived : public Base {
            int y;
            void setY(int v) { this->y = v; }
        };
        
        int main() {
            Derived d;
            d.setX(10);
            d.setY(20);
            return d.x + d.y;
        }
    )", "test_inherit.c"), 30);
}
```

- [ ] **Step 2: Run E2E tests to verify they pass**

Run: `cd build && cmake --build . && ctest -R ClassE2E --output-on-failure`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add tests/e2e/test_class_support.cpp
git commit -m "test: add E2E tests for class support"
```

---

### Task 7: Final Verification

**Files:**
- No new files (verification only)

**Interfaces:**
- Consumes: All completed tasks from 1-6
- Produces: Confirms all tests pass

- [ ] **Step 1: Run all tests**

Run: `cd build && ctest --output-on-failure`
Expected: All 415+ tests pass, 0 failed

- [ ] **Step 2: Verify no regressions**

Check that existing struct, union, enum tests still pass

- [ ] **Step 3: Update plan completion status**

```bash
echo "Task 7: Final verification complete" >> docs/superpowers/plans/2026-08-05-class-support.md
```

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/plans/2026-08-05-class-support.md
git commit -m "docs: mark class support implementation complete"
```
