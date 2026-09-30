#pragma once
#include "shader_lab/lab.hpp"
namespace Libs::Graphics::ShaderRecompiler::IR {
struct Program;
struct ResourceSnapshot;
} // namespace Libs::Graphics::ShaderRecompiler::IR
namespace sl {
// Private upstream-dependent interface. Public consumers use the versioned JSON artifact.
json compiler_layout(const Libs::Graphics::ShaderRecompiler::IR::Program &program,
                     const Libs::Graphics::ShaderRecompiler::IR::ResourceSnapshot &snapshot);
} // namespace sl
