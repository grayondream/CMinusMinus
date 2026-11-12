#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "driver/CompilerDriver.h"

#include "llvm/Support/TargetSelect.h"

class CompilerDriverTest : public ::testing::Test {
protected:
    void SetUp() override {
        static bool init = false;
        if (!init) {
            llvm::InitializeAllTargetInfos();
            llvm::InitializeAllTargets();
            llvm::InitializeAllTargetMCs();
            llvm::InitializeAllAsmParsers();
            llvm::InitializeAllAsmPrinters();
            init = true;
        }
        driver = CompilerDriver();
    }

    CompilerDriver driver;
};

TEST_F(CompilerDriverTest, ParseCompileOnlyFlag) {
    const char* argv[] = {"cdd", "-c", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(3, argv));
    EXPECT_TRUE(driver.getCompileOnly());
}

TEST_F(CompilerDriverTest, ParseOutputFlag) {
    const char* argv[] = {"cdd", "-o", "out.o", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(4, argv));
    EXPECT_EQ(driver.getOutputFile(), "out.o");
}

TEST_F(CompilerDriverTest, ParseSingleModulePath) {
    const char* argv[] = {"cdd", "-M", "libs", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(4, argv));
    ASSERT_EQ(driver.getModulePaths().size(), 1u);
    EXPECT_EQ(driver.getModulePaths()[0], "libs");
}

TEST_F(CompilerDriverTest, ParseMultipleModulePaths) {
    const char* argv[] = {"cdd", "-M", "libs", "-M", "vendored", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(6, argv));
    ASSERT_EQ(driver.getModulePaths().size(), 2u);
    EXPECT_EQ(driver.getModulePaths()[0], "libs");
    EXPECT_EQ(driver.getModulePaths()[1], "vendored");
}

TEST_F(CompilerDriverTest, DefaultBehaviorIsJitMode) {
    const char* argv[] = {"cdd"};
    ASSERT_TRUE(driver.parseArguments(1, argv));
    EXPECT_TRUE(driver.getJitMode());
    EXPECT_TRUE(driver.getInputFiles().empty());
}

TEST_F(CompilerDriverTest, ParseInputFile) {
    const char* argv[] = {"cdd", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(2, argv));
    ASSERT_EQ(driver.getInputFiles().size(), 1u);
    EXPECT_EQ(driver.getInputFiles()[0], "test.cdd");
    EXPECT_FALSE(driver.getJitMode());
}

TEST_F(CompilerDriverTest, ParseMultipleInputFiles) {
    const char* argv[] = {"cdd", "a.cdd", "b.cdd"};
    ASSERT_TRUE(driver.parseArguments(3, argv));
    ASSERT_EQ(driver.getInputFiles().size(), 2u);
    EXPECT_EQ(driver.getInputFiles()[0], "a.cdd");
    EXPECT_EQ(driver.getInputFiles()[1], "b.cdd");
}

TEST_F(CompilerDriverTest, ParseEmitIR) {
    const char* argv[] = {"cdd", "-S", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(3, argv));
    EXPECT_TRUE(driver.getEmitIR());
}

TEST_F(CompilerDriverTest, ParseSyntaxOnly) {
    const char* argv[] = {"cdd", "-fsyntax-only", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(3, argv));
    EXPECT_TRUE(driver.getSyntaxOnly());
}

TEST_F(CompilerDriverTest, ParseWall) {
    const char* argv[] = {"cdd", "-Wall", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(3, argv));
    EXPECT_TRUE(driver.getWall());
}

TEST_F(CompilerDriverTest, ParseWerror) {
    const char* argv[] = {"cdd", "-Werror", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(3, argv));
    EXPECT_TRUE(driver.getWerror());
}

TEST_F(CompilerDriverTest, ParseStdStandard) {
    const char* argv[] = {"cdd", "-std", "c11", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(4, argv));
    EXPECT_EQ(driver.getStdStandard(), "c11");
}

TEST_F(CompilerDriverTest, ParseOptLevels) {
    {
        const char* argv[] = {"cdd", "-O", "0", "test.cdd"};
        ASSERT_TRUE(driver.parseArguments(4, argv));
        EXPECT_EQ(driver.getOptLevel(), 0);
    }
    {
        CompilerDriver d;
        const char* argv[] = {"cdd", "-O", "2", "test.cdd"};
        ASSERT_TRUE(d.parseArguments(4, argv));
        EXPECT_EQ(d.getOptLevel(), 2);
    }
    {
        CompilerDriver d;
        const char* argv[] = {"cdd", "-O", "3", "test.cdd"};
        ASSERT_TRUE(d.parseArguments(4, argv));
        EXPECT_EQ(d.getOptLevel(), 3);
    }
}

TEST_F(CompilerDriverTest, ParseDebugFlag) {
    const char* argv[] = {"cdd", "-g", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(3, argv));
    EXPECT_TRUE(driver.getIncludeDebug());
}

TEST_F(CompilerDriverTest, ParseVerboseFlag) {
    const char* argv[] = {"cdd", "-v", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(3, argv));
    EXPECT_TRUE(driver.getVerbose());
}

TEST_F(CompilerDriverTest, ParseLib) {
    const char* argv[] = {"cdd", "-l", "m", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(4, argv));
    ASSERT_EQ(driver.getLibs().size(), 1u);
    EXPECT_EQ(driver.getLibs()[0], "m");
}

TEST_F(CompilerDriverTest, ParseMultipleLibs) {
    const char* argv[] = {"cdd", "-l", "m", "-l", "pthread", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(6, argv));
    ASSERT_EQ(driver.getLibs().size(), 2u);
    EXPECT_EQ(driver.getLibs()[0], "m");
    EXPECT_EQ(driver.getLibs()[1], "pthread");
}

TEST_F(CompilerDriverTest, ParseLibPath) {
    const char* argv[] = {"cdd", "-L", "/usr/lib", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(4, argv));
    ASSERT_EQ(driver.getLibPaths().size(), 1u);
    EXPECT_EQ(driver.getLibPaths()[0], "/usr/lib");
}

TEST_F(CompilerDriverTest, ParseMultipleLibPaths) {
    const char* argv[] = {"cdd", "-L", "/usr/lib", "-L", "/usr/local/lib", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(6, argv));
    ASSERT_EQ(driver.getLibPaths().size(), 2u);
    EXPECT_EQ(driver.getLibPaths()[0], "/usr/lib");
    EXPECT_EQ(driver.getLibPaths()[1], "/usr/local/lib");
}

TEST_F(CompilerDriverTest, ParseCombinedFlags) {
    const char* argv[] = {"cdd", "-c", "-g", "-O", "2", "-M", "libs", "-o", "out.o", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(10, argv));
    EXPECT_TRUE(driver.getCompileOnly());
    EXPECT_TRUE(driver.getIncludeDebug());
    EXPECT_EQ(driver.getOptLevel(), 2);
    ASSERT_EQ(driver.getModulePaths().size(), 1u);
    EXPECT_EQ(driver.getModulePaths()[0], "libs");
    EXPECT_EQ(driver.getOutputFile(), "out.o");
    ASSERT_EQ(driver.getInputFiles().size(), 1u);
    EXPECT_EQ(driver.getInputFiles()[0], "test.cdd");
}

TEST_F(CompilerDriverTest, UnknownOptionFails) {
    const char* argv[] = {"cdd", "--unknown", "test.cdd"};
    EXPECT_FALSE(driver.parseArguments(3, argv));
}

TEST_F(CompilerDriverTest, MissingOutputArgumentFails) {
    const char* argv[] = {"cdd", "-o"};
    EXPECT_FALSE(driver.parseArguments(2, argv));
}

TEST_F(CompilerDriverTest, RunReturnsZero) {
    const char* argv[] = {"cdd"};
    ASSERT_TRUE(driver.parseArguments(1, argv));
    EXPECT_EQ(driver.run(), 0);
}

TEST_F(CompilerDriverTest, RunJitModeReturnsZero) {
    const char* argv[] = {"cdd"};
    ASSERT_TRUE(driver.parseArguments(1, argv));
    EXPECT_EQ(driver.run(), 0);
}

TEST_F(CompilerDriverTest, DefaultOutputIsAOut) {
    const char* argv[] = {"cdd", "test.cdd"};
    ASSERT_TRUE(driver.parseArguments(2, argv));
    EXPECT_EQ(driver.getOutputFile(), "a.out");
}

TEST_F(CompilerDriverTest, PrintHelpUsesCDD) {
    testing::internal::CaptureStdout();
    driver.printHelp();
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("Usage: cdd"), std::string::npos);
}

namespace {
std::filesystem::path writeTempSource(const std::string& name, const std::string& text) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / name;
    std::ofstream out(p);
    out << text;
    return p;
}
} // namespace

TEST_F(CompilerDriverTest, RejectsLegacySmcInput) {
    const std::filesystem::path p =
        writeTempSource("legacy_input.smc", "int32 main() { return 0; }\n");
    const std::string path = p.string();
    const char* argv[] = {"cdd", "-fsyntax-only", path.c_str()};
    ASSERT_TRUE(driver.parseArguments(3, argv));
    testing::internal::CaptureStderr();
    EXPECT_EQ(driver.run(), 1);
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("legacy .smc source is not supported"), std::string::npos);
    std::filesystem::remove(p);
}

TEST_F(CompilerDriverTest, BareInputSearchesItsDirectory) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("cdd_driver_bare_input_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "util.cdd");
        out << "module util;\nexport int32 helper() { return 5; }\n";
    }
    {
        std::ofstream out(dir / "main.cdd");
        out << "module main;\nimport util;\n"
               "int32 main() { return helper(); }\n";
    }
    const std::filesystem::path old = std::filesystem::current_path();
    std::filesystem::current_path(dir);
    const char* argv[] = {"cdd", "-fsyntax-only", "main.cdd"};
    const bool parsed = driver.parseArguments(3, argv);
    const int result = parsed ? driver.run() : -1;
    std::filesystem::current_path(old);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(result, 0);
    std::filesystem::remove_all(dir);
}

TEST_F(CompilerDriverTest, RejectsLegacySmcHiddenFile) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("cdd_driver_hidden_smc_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path p = dir / ".smc";
    {
        std::ofstream out(p);
        out << "int32 main() { return 0; }\n";
    }
    const std::string path = p.string();
    const char* argv[] = {"cdd", "-fsyntax-only", path.c_str()};
    ASSERT_TRUE(driver.parseArguments(3, argv));
    testing::internal::CaptureStderr();
    EXPECT_EQ(driver.run(), 1);
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("legacy .smc source is not supported"), std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_F(CompilerDriverTest, LexSyntaxOnlySucceedsForValidSource) {
    const std::filesystem::path p =
        writeTempSource("cdd_driver_valid.cdd", "int32 main() { return 0; }\n");
    const std::string path = p.string();
    const char* argv[] = {"cdd", "-fsyntax-only", path.c_str()};
    ASSERT_TRUE(driver.parseArguments(3, argv));
    EXPECT_EQ(driver.run(), 0);
    std::filesystem::remove(p);
}

TEST_F(CompilerDriverTest, InvalidCharacterFailsCompilation) {
    // LEX-13: the driver must surface lexer diagnostics and fail the build.
    const std::filesystem::path p = writeTempSource("cdd_driver_badlex.cdd", "int32 @ x;\n");
    const std::string path = p.string();
    const char* argv[] = {"cdd", "-fsyntax-only", path.c_str()};
    ASSERT_TRUE(driver.parseArguments(3, argv));
    EXPECT_EQ(driver.run(), 1);
    std::filesystem::remove(p);
}

TEST_F(CompilerDriverTest, ThreadProgramLinksAndRuns) {
    // std.thread requires the driver to append -l pthread for generated
    // programs; this exercises the real compile+link path, not the JIT.
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("cdd_thread_driver_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const std::filesystem::path src = dir / "thread.cdd";
    const std::filesystem::path out = dir / "thread";
    {
        std::ofstream f(src);
        f << "import std.thread;\n"
             "int64 seen = 0;\n"
             "void* worker(void* arg) { seen = (int64)arg; return null; }\n"
             "int32 main() {\n"
             "    std::Thread t;\n"
             "    t.init();\n"
             "    if (!t.start(worker, (void*)5)) return 1;\n"
             "    if (!t.join()) return 2;\n"
             "    return seen == 5 ? 0 : 3;\n"
             "}\n";
    }
    const std::string srcPath = src.string();
    const std::string outPath = out.string();
    const char* argv[] = {"cdd", "-M", STD_DIR, srcPath.c_str(),
                          "-o", outPath.c_str()};
    ASSERT_TRUE(driver.parseArguments(6, argv));
    EXPECT_EQ(driver.run(), 0);
    EXPECT_EQ(std::system(outPath.c_str()), 0);
    std::filesystem::remove_all(dir);
}
