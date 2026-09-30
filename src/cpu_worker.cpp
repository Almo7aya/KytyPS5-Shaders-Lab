#include "shader_lab/cpu_reference.hpp"
#include <iostream>
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout
                << "shader-cpu-reference --execute-fixture REQUEST_JSON\n"
                   "shader-cpu-reference --generate-fixtures REQUEST_JSON (internal workflow)\n"
                   "Bounded independent RDNA2 integer model; not hardware conformance.\n";
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--generate-fixtures")
            return sl::cpu_generate_fixtures(sl::path_from(argv[2]));
        if (argc != 3 || std::string(argv[1]) != "--execute-fixture")
            throw std::runtime_error("expected --execute-fixture REQUEST_JSON");
        return sl::cpu_reference_worker(sl::path_from(argv[2]));
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
