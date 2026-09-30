#pragma once
#include "shader_lab/lab.hpp"

namespace Libs::Graphics::ShaderRecompiler::IR {
struct Program;
}
namespace sl {
// The catalog is versioned separately from the worker protocol. A boundary means
// a pass returned, not that its IR or generated shader is semantically correct.
json compiler_pass_catalog();
struct CompilerCheckpointStop {
    unsigned index;
};
void compiler_pass(unsigned index, bool completed,
                   const Libs::Graphics::ShaderRecompiler::IR::Program *ir = nullptr);
} // namespace sl
