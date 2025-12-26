#include "TestFramework.h"

#include "Rigel/Asset/ShaderCompiler.h"

#include <unordered_map>

using namespace Rigel::Asset;

TEST_CASE(ShaderCompiler_PreprocessAddsDefines) {
    ShaderSource source;
    source.vertex = "#version 330 core\nvoid main(){}";
    source.defines["FOO"] = "1";
    source.defines["BAR"] = "true";

    std::string out = ShaderCompiler::preprocess(source.vertex, source.defines);
    CHECK(out.find("#version 330 core") == 0);
    CHECK(out.find("#define FOO 1") != std::string::npos);
    CHECK(out.find("#define BAR 1") != std::string::npos);
}

TEST_CASE(ShaderCompiler_PreprocessInsertsVersion) {
    std::string src = "void main(){}";
    std::unordered_map<std::string, std::string> defines;
    defines["BAZ"] = "2";

    std::string out = ShaderCompiler::preprocess(src, defines);
    CHECK(out.find("#version 410 core") == 0);
    CHECK(out.find("#define BAZ 2") != std::string::npos);
}
