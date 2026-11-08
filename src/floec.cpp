// floec: the Floe ahead-of-time compiler (Phase 2).
//
//   source.floe --[Flex/Bison + sema + LLVM IRBuilder]--> LLVM IR
//               --[LLVM backend]--> object file --[cc + floe_runtime.o]--> executable

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>

#include "codegen.h"
#include "driver.h"

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: floec [options] <file.floe>\n"
                 "\n"
                 "Floe ahead-of-time compiler (Phase 2). Options:\n"
                 "  -o <path>     name of the output file\n"
                 "  --emit-llvm   write LLVM IR (.ll) instead of an executable\n"
                 "  -c            write a native object file (.o) instead of an executable\n"
                 "  -O            optimise (LLVM -O2 pipeline)\n"
                 "\n"
                 "Environment: CC (linker, default cc), FLOE_RUNTIME (path to floe_runtime.o)\n");
}

// "dir/prog.floe" -> "dir/prog"
std::string strip_extension(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return path.substr(0, dot);
    }
    return path + ".out";
}

// floe_runtime.o is installed next to the floec executable.
std::string runtime_object() {
    if (const char* env = std::getenv("FLOE_RUNTIME")) {
        return env;
    }
    char self[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) {
        return "floe_runtime.o";
    }
    std::string dir(self, static_cast<size_t>(n));
    return dir.substr(0, dir.find_last_of('/') + 1) + "floe_runtime.o";
}

// Run a program without a shell (paths may contain spaces). Returns its exit status.
int run(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(args[0], args.data());
        std::perror(args[0]);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string input, output;
    bool emit_llvm = false, object_only = false, optimize = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "--emit-llvm") {
            emit_llvm = true;
        } else if (arg == "-c") {
            object_only = true;
        } else if (arg == "-O" || arg == "-O2") {
            optimize = true;
        } else if (!arg.empty() && arg[0] != '-' && input.empty()) {
            input = arg;
        } else {
            usage();
            return 2;
        }
    }
    if (input.empty() || (emit_llvm && object_only)) {
        usage();
        return 2;
    }
    if (output.empty()) {
        output = strip_extension(input) + (emit_llvm ? ".ll" : object_only ? ".o" : "");
    }

    // Parse; the Bison action hands every top-level item to the code generator.
    floe::CodeGenerator codegen(input.c_str());
    floe::ParseResult parsed = floe::parse_file(input.c_str(), &codegen);
    if (!parsed.program) return 2;
    if (!parsed.ok || codegen.failed() || !codegen.finish()) {
        std::fprintf(stderr, "%s: compilation failed\n", input.c_str());
        return 1;
    }
    if (optimize) {
        codegen.optimize();
    }

    if (emit_llvm) {
        return codegen.write_ir(output) ? 0 : 1;
    }
    const std::string object = object_only ? output : output + ".o";
    if (!codegen.write_object(object)) {
        return 1;
    }
    if (object_only) {
        return 0;
    }

    const char* cc = std::getenv("CC");
    const int status = run({cc != nullptr ? cc : "cc", object, runtime_object(), "-o", output, "-lm"});
    std::remove(object.c_str());
    if (status != 0) {
        std::fprintf(stderr, "%s: linking failed (is floe_runtime.o at %s?)\n", input.c_str(),
                     runtime_object().c_str());
        return 1;
    }
    return 0;
}
