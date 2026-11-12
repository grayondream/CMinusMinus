#include "CompilerDriver.h"
#include "Linker.h"
#include "ModuleLoader.h"
#include "StdPrelude.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "codegen/CodegenContext.h"
#include "support/File.h"
#include "support/Log.h"
#include <cxxopts.hpp>
#include <filesystem>
#include <iostream>
#include <set>
#include <cstdlib>

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Config/llvm-config.h"

static void appendStdLibDirs(std::vector<std::string>& dirs) {
    auto add = [&](const std::filesystem::path& p) {
        if (p.empty()) return;
        std::error_code ec;
        if (!std::filesystem::is_directory(p, ec)) return;
        std::string s = p.string();
        for (const auto& existing : dirs) {
            if (existing == s) return;
        }
        dirs.push_back(std::move(s));
    };
    if (const char* env = std::getenv("CDD_STD_DIR")) add(env);
#ifdef STD_DIR
    add(STD_DIR);
#endif
#ifdef CDD_INSTALL_STD_DIR
    add(CDD_INSTALL_STD_DIR);
#endif
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        auto base = exe.parent_path();
        add(base / ".." / "share" / "cdd" / "libs");
        add(base / ".." / "lib" / "cdd" / "libs");
    }
}

static std::string getHostTargetTriple() {
    return llvm::sys::getDefaultTargetTriple();
}

static std::unique_ptr<llvm::TargetMachine>
createTargetMachine(llvm::Module& module) {
    std::string triple = getHostTargetTriple();
    llvm::Triple llvmTriple(triple);
    // LLVM 19 changed Module::setTargetTriple/Target::createTargetMachine to
    // take a llvm::Triple instead of its string form.
#if LLVM_VERSION_MAJOR >= 19
    module.setTargetTriple(llvmTriple);
#else
    module.setTargetTriple(llvmTriple.str());
#endif

    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(llvmTriple.str(), error);
    if (!target) {
        LOGE("Target lookup failed: {}", error);
        return nullptr;
    }

    llvm::TargetOptions options;
#if LLVM_VERSION_MAJOR >= 19
    auto tm = target->createTargetMachine(
        llvmTriple, "generic", "", options, llvm::Reloc::PIC_);
#else
    auto tm = target->createTargetMachine(
        llvmTriple.str(), "generic", "", options, llvm::Reloc::PIC_);
#endif
    if (!tm) {
        LOGE("Failed to create TargetMachine");
        return nullptr;
    }

    module.setDataLayout(tm->createDataLayout());
    return std::unique_ptr<llvm::TargetMachine>(tm);
}

static bool emitObjectFile(llvm::Module& module, const std::string& outputPath) {
    auto tm = createTargetMachine(module);
    if (!tm) return false;

    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        LOGE("Failed to open output file: {} ({})", outputPath, ec.message());
        return false;
    }

    llvm::legacy::PassManager passManager;
    if (tm->addPassesToEmitFile(passManager, dest, nullptr,
            llvm::CodeGenFileType::ObjectFile)) {
        LOGE("TargetMachine cannot emit object file");
        return false;
    }

    passManager.run(module);
    dest.flush();
    dest.close();
    return true;
}

bool CompilerDriver::parseArguments(int argc, const char* argv[]) {
    inputFiles.clear();
    outputFile.clear();
    modulePaths.clear();
    libs.clear();
    libPaths.clear();
    compileOnly = false;
    emitIR = false;
    emitObj = false;
    syntaxOnly = false;
    jitMode = false;
    usePrelude = true;
    verbose = false;
    wall = false;
    werror = false;
    optLevel = 0;
    includeDebug = false;
    stdStandard.clear();

    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-Wall") {
            wall = true;
        } else if (arg == "-Werror") {
            werror = true;
        } else if (arg.substr(0, 5) == "-std=") {
            stdStandard = arg.substr(5);
        } else if (arg == "-std" && i + 1 < argc) {
            stdStandard = argv[++i];
        } else if (arg == "-fsyntax-only") {
            syntaxOnly = true;
        } else {
            args.push_back(arg);
        }
    }

    std::vector<const char*> cArgs;
    cArgs.push_back("cdd");
    for (const auto& a : args) {
        cArgs.push_back(a.c_str());
    }
    int newArgc = static_cast<int>(cArgs.size());
    const char** newArgv = cArgs.data();

    cxxopts::Options options("cdd", "The CDD compiler (C dot dot)");
    
    options.add_options()
        ("c", "Compile only, do not link")
        ("o,output", "Output file", cxxopts::value<std::string>())
        ("S", "Emit IR only")
        ("M,module-path", "Add module import search path", cxxopts::value<std::vector<std::string>>())
        ("O", "Optimization level (0-3)", cxxopts::value<int>())
        ("g", "Include debug information")
        ("v,verbose", "Verbose output")
        ("no-prelude", "Do not inject the built-in std.c libc binding layer")
        ("l", "Link library", cxxopts::value<std::vector<std::string>>())
        ("L", "Library search path", cxxopts::value<std::vector<std::string>>())
        ("h,help", "Show this help message")
        ("positional", "Input files", cxxopts::value<std::vector<std::string>>())
        ;

    options.parse_positional({"positional"});

    try {
        auto result = options.parse(newArgc, newArgv);

        if (result.count("help")) {
            printHelp();
            return false;
        }

        if (result.count("c")) compileOnly = true;
        if (result.count("S")) emitIR = true;
        if (result.count("g")) includeDebug = true;
        if (result.count("v")) verbose = true;
        if (result.count("no-prelude")) usePrelude = false;

        if (result.count("o")) outputFile = result["o"].as<std::string>();
        if (result.count("O")) optLevel = result["O"].as<int>();

        if (result.count("M")) modulePaths = result["M"].as<std::vector<std::string>>();
        if (result.count("l")) libs = result["l"].as<std::vector<std::string>>();
        if (result.count("L")) libPaths = result["L"].as<std::vector<std::string>>();

        if (result.count("positional")) {
            inputFiles = result["positional"].as<std::vector<std::string>>();
        }
    } catch (const cxxopts::exceptions::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return false;
    }

    if (inputFiles.empty()) {
        jitMode = true;
    }

    if (outputFile.empty() && !inputFiles.empty()) {
        outputFile = "a.out";
    }

    return true;
}

int CompilerDriver::run() {
    if (jitMode) {
        if (verbose) std::cout << "running in JIT mode\n";
        return 0;
    }

    int exitCode = 0;
    for (const auto& file : inputFiles) {
        if (compileFile(file) != 0) {
            exitCode = 1;
        }
    }
    return exitCode;
}

int CompilerDriver::compileFile(const std::string& inputFile) {
    // Match the loader's suffix test rather than path::extension(), which
    // treats a bare ".smc" filename as a basename with no extension.
    const std::string inputName =
        std::filesystem::path(inputFile).filename().string();
    if (inputName.size() >= 4 &&
        inputName.compare(inputName.size() - 4, 4, ".smc") == 0) {
        std::cerr << "legacy .smc source is not supported; rename it to .cdd\n";
        return 1;
    }

    if (verbose) {
        std::cout << "compiling: " << inputFile << "\n";
        std::cout << "  output: " << outputFile << "\n";
        std::cout << "  compile-only: " << compileOnly << "\n";
        std::cout << "  emit-ir: " << emitIR << "\n";
        std::cout << "  opt-level: " << optLevel << "\n";
        std::cout << "  include-debug: " << includeDebug << "\n";
    }

    File file(inputFile.c_str());
    if (!file.isOpen()) {
        LOGE("open file {} failed", inputFile);
        return 1;
    }

    std::string content = file.readAll();
    file.close();

    if (content.empty()) {
        LOGE("file {} is empty", inputFile);
        return 1;
    }

    Lexer lexer(inputFile, content);
    auto tokens = lexer.tokenize();

    // LEX-13 / LEX-17: surface lexical diagnostics (E0001/E0002/E0003) and stop
    // before parsing; scanning already recovered so later tokens stay usable.
    bool lexError = false;
    for (const auto& diag : lexer.getDiagnostics()) {
        std::cerr << diag.formatWithSeverity() << "\n";
        if (diag.isError()) {
            lexError = true;
        }
    }
    if (lexError) {
        return 1;
    }

    // P0-04/MOD-04: load imports before parsing the importer so imported
    // types are registered in TypeContext when `S s;` is parsed. Functions and
    // variables already worked through sema; types need this parse-time pass.
    std::set<std::string> loadedModules;
    cdd::ModuleSearchPaths paths;
    std::vector<std::unique_ptr<DeclAST>> importedDecls;
    std::vector<std::string> importErrors;
    std::string importerDir =
        std::filesystem::path(inputFile).parent_path().string();
    if (importerDir.empty()) {
        // A bare input name has an empty parent_path(); its directory is the
        // current directory, which module imports search by default.
        std::error_code cwdEc;
        auto cwd = std::filesystem::current_path(cwdEc);
        if (!cwdEc) importerDir = cwd.string();
    }
    {
        std::error_code canonEc;
        loadedModules.insert(
            std::filesystem::weakly_canonical(std::filesystem::path(inputFile), canonEc).string());
        for (const auto& dir : modulePaths) {
            paths.dirs.push_back(dir);
        }
        appendStdLibDirs(paths.dirs);
        auto specs = cdd::collectImportSpecifiers(tokens);
        cdd::preloadImports(specs, importerDir, paths, loadedModules, importedDecls, importErrors);
        if (!importErrors.empty()) {
            for (const auto& err : importErrors) {
                std::cerr << err << "\n";
            }
            return 1;
        }
    }

    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) {
        LOGE("parse ast failed");
        return 1;
    }
    if (!parser.getErrors().empty()) {
        for (const auto& diag : parser.getErrors()) {
            std::cerr << diag.formatWithSeverity() << "\n";
        }
        return 1;
    }

    // MOD-13: the entry unit's module name must match its file path, mirroring
    // the check loadModuleFile performs for imported modules.
    if (!ast->moduleName.empty()) {
        std::filesystem::path inputPath(inputFile);
        std::string module = ast->moduleName;
        bool matches = false;
        if (module.find('.') == std::string::npos) {
            matches = inputPath.stem().string() == module;
        } else {
            std::string relative = module;
            for (char& c : relative) {
                if (c == '.') c = '/';
            }
            std::string canonical =
                std::filesystem::weakly_canonical(inputPath).string();
            std::string ext = inputPath.extension().string();
            std::string suffix = "/" + relative + ext;
            matches = canonical.size() >= suffix.size() &&
                      canonical.compare(canonical.size() - suffix.size(), suffix.size(),
                                        suffix) == 0;
        }
        if (!matches) {
            std::cerr << inputFile << ": module declares name '" << module
                      << "' but the file name does not match\n";
            return 1;
        }
    }

    if (!importedDecls.empty()) {
        ast->declarations.insert(
            ast->declarations.begin(),
            std::make_move_iterator(importedDecls.begin()),
            std::make_move_iterator(importedDecls.end()));
    }
    // Keep the existing post-parse path as a safety net; `loadedModules`
    // deduplicates anything preloaded above.
    cdd::processImports(*ast, importerDir, paths, loadedModules, importErrors);
    if (!importErrors.empty()) {
        for (const auto& err : importErrors) {
            std::cerr << err << "\n";
        }
        return 1;
    }

    if (syntaxOnly) {
        return 0;
    }

    // Inject std.c (MOD-09 / STD-23) before semantic analysis so its
    // declarations precede user code. The binding layer is a standard-library
    // file (libs/std/c.cdd), never compiler-embedded source.
    if (usePrelude) {
        std::vector<std::string> stdDirs;
        for (const auto& dir : modulePaths) {
            stdDirs.push_back(dir);
        }
        appendStdLibDirs(stdDirs);
        std::string preludeSource = cdd::loadStdCPrelude(stdDirs);
        if (preludeSource.empty()) {
            std::cerr << "error: standard library not found (expected <std>/c.cdd); "
                         "install the stdlib or pass -M, or use --no-prelude\n";
            return 1;
        }
        if (auto prelude = cdd::parseStdCPrelude(preludeSource, "<std.c>")) {
            cdd::prependDeclarations(*ast, *prelude);
        }
    }

    SemanticAnalyzer analyzer;
    // P1-04 / CT-05: build 查询注入（optLevel>0 记为 Ox；debug 暂无 CLI flag）。
    analyzer.setBuildConfig(false, optLevel > 0 ? "O" + std::to_string(optLevel) : "O0");
    analyzer.analyze(*ast);
    for (const auto& diag : analyzer.getErrors()) {
        std::cerr << diag.formatWithSeverity() << "\n";
    }
    for (const auto& diag : analyzer.getWarnings()) {
        std::cerr << diag.formatWithSeverity() << "\n";
    }
    if (!analyzer.getErrors().empty() ||
        (werror && !analyzer.getWarnings().empty())) {
        return 1;
    }

    CodegenContext codegenCtx;
    codegenCtx.setSourceFile(inputFile);
    ast->codegen(codegenCtx);
    codegenCtx.finalizeDebugInfo();

    if (emitIR) {
        codegenCtx.getModule().print(llvm::outs(), nullptr);
        return 0;
    }

    if (optLevel > 0) {
        llvm::LoopAnalysisManager LAM;
        llvm::FunctionAnalysisManager FAM;
        llvm::CGSCCAnalysisManager CGAM;
        llvm::ModuleAnalysisManager MAM;

        llvm::PassBuilder PB;
        PB.registerModuleAnalyses(MAM);
        PB.registerCGSCCAnalyses(CGAM);
        PB.registerFunctionAnalyses(FAM);
        PB.registerLoopAnalyses(LAM);
        PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

        llvm::OptimizationLevel optLevelEnum;
        switch (optLevel) {
            case 1: optLevelEnum = llvm::OptimizationLevel::O1; break;
            case 2: optLevelEnum = llvm::OptimizationLevel::O2; break;
            case 3: optLevelEnum = llvm::OptimizationLevel::O3; break;
            default: optLevelEnum = llvm::OptimizationLevel::O2; break;
        }

        llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(optLevelEnum);
        MPM.run(codegenCtx.getModule(), MAM);
    }

    if (compileOnly) {
        std::string objectPath = outputFile;
        if (!emitObjectFile(codegenCtx.getModule(), objectPath)) {
            LOGE("failed to emit object file");
            return 1;
        }
        return 0;
    }

    std::string objectPath = outputFile + ".o";
    if (!emitObjectFile(codegenCtx.getModule(), objectPath)) {
        LOGE("failed to emit object file");
        return 1;
    }

    // Link user-requested -l/-L and the bundled mimalloc allocator. The
    // allocator is placed after the generated object and user libraries so the
    // static archive can satisfy malloc/free before libc is searched.
    std::vector<std::string> linkLibs = libs;
    std::vector<std::string> linkPaths = libPaths;
    auto addMimalloc = [&](const std::string& dir) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(
                std::filesystem::path(dir) / "libmimalloc.a", ec)) {
            return false;
        }
        for (const auto& p : linkPaths) {
            if (p == dir) return true;
        }
        linkPaths.push_back(dir);
        return true;
    };
    bool haveMimalloc = false;
#ifdef MIMALLOC_LIB_DIR
    haveMimalloc = addMimalloc(MIMALLOC_LIB_DIR);
#endif
#ifdef CDD_INSTALL_MIMALLOC_DIR
    if (!haveMimalloc) haveMimalloc = addMimalloc(CDD_INSTALL_MIMALLOC_DIR);
#endif
    if (!haveMimalloc) {
        std::error_code ec;
        auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            auto base = exe.parent_path();
            haveMimalloc = addMimalloc((base / ".." / "lib").string()) ||
                           addMimalloc((base / ".." / "lib64").string());
        }
    }
    if (haveMimalloc) linkLibs.push_back("mimalloc");
    // std.math binds libm; without this the documented module fails to link.
    linkLibs.push_back("m");
    // std.thread binds pthread; glibc 2.34+ folds it into libc, but keep the
    // explicit library for older toolchains and deterministic behavior.
    linkLibs.push_back("pthread");

    Linker linker;
    int ret = linker.link({objectPath}, outputFile, linkLibs, linkPaths);
    if (ret != 0) {
        LOGE("linker failed with exit code {}", ret);
        return ret;
    }

    std::remove(objectPath.c_str());

    return 0;
}

void CompilerDriver::printHelp() const {
    std::cout << "Usage: cdd [options] <input files>\n"
              << "\n"
              << "Options:\n"
              << "  -c              Compile only, do not link\n"
              << "  -o <file>       Output file\n"
              << "  -S              Emit IR only\n"
              << "  -M <path>       Add module import search path\n"
              << "  -O <level>      Optimization level (0-3)\n"
              << "  -g              Include debug information\n"
              << "  -v              Verbose output\n"
              << "  --no-prelude    Do not inject the built-in std.c binding layer\n"
              << "  -Wall           Enable all warnings\n"
              << "  -Werror         Treat warnings as errors\n"
              << "  -std <standard> C standard (c99, c11, c17)\n"
              << "  -fsyntax-only   Syntax check only\n"
              << "  -l <lib>        Link library\n"
              << "  -L <path>       Library search path\n"
              << "  -h, --help      Show this help message\n";
}
