// Thin process boundary. The compiler implementation lives in shader_lab_kyty_compiler.
#include "shader_lab/compiler.hpp"
#include <iostream>

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--compiler-info") {
        std::cout << sl::compiler_info_v1().dump(2) << "\n";
        return 0;
    }
    if (argc != 3 || std::string_view(argv[1]) != "--request") {
        std::cerr << "shader-kyty-worker --request request.json\n"
                     "shader-kyty-worker --compiler-info\n";
        return 2;
    }
    return sl::execute_compiler_request_v1(sl::path_from(argv[2]));
}
