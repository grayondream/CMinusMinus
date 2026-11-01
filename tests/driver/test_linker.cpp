#include <gtest/gtest.h>
#include "driver/Linker.h"

TEST(LinkerTest, FindLinker) {
    Linker linker;
    std::string path = linker.findSystemLinker();
    EXPECT_FALSE(path.empty());
}

TEST(LinkerTest, ConstructLinkCommand) {
    Linker linker;
    std::vector<std::string> objects = {"a.o", "b.o"};
    std::string output = "program";
    std::string cmd = linker.constructLinkCommand(objects, output);
    EXPECT_NE(cmd.find("a.o"), std::string::npos);
    EXPECT_NE(cmd.find("b.o"), std::string::npos);
    EXPECT_NE(cmd.find("-o program"), std::string::npos);
}

TEST(LinkerTest, ConstructLinkCommandHonorsLibsAndLibPaths) {
    Linker linker;
    std::string cmd = linker.constructLinkCommand(
        {"main.o"}, "app", {"m", "mimalloc"}, {"/opt/lib", "/usr/local/lib"});
    EXPECT_NE(cmd.find("-L/opt/lib"), std::string::npos);
    EXPECT_NE(cmd.find("-L/usr/local/lib"), std::string::npos);
    EXPECT_NE(cmd.find("-lm"), std::string::npos);
    EXPECT_NE(cmd.find("-lmimalloc"), std::string::npos);
    EXPECT_LT(cmd.find("main.o"), cmd.find("-lmimalloc"));
}

TEST(LinkerTest, ConstructLinkCommandQuotesPathsWithSpaces) {
    Linker linker;
    std::string cmd = linker.constructLinkCommand(
        {"my object.o"}, "out dir/app", {"m"}, {"/opt/my libs"});
    EXPECT_NE(cmd.find("'my object.o'"), std::string::npos);
    EXPECT_NE(cmd.find("'out dir/app'"), std::string::npos);
    EXPECT_NE(cmd.find("-L'/opt/my libs'"), std::string::npos);
}
