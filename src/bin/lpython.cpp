#include <chrono>
#include <iostream>
#include <stdlib.h>
#include <cstdlib>

#define CLI11_HAS_FILESYSTEM 0
#include <bin/CLI11.hpp>

#include <libasr/stacktrace.h>
#include <lpython/pickle.h>
#include <lpython/semantics/python_ast_to_asr.h>
#include <libasr/codegen/asr_to_llvm.h>
#include <libasr/codegen/asr_to_cpp.h>
#include <libasr/codegen/asr_to_py.h>
#include <libasr/codegen/asr_to_x86.h>
#include <lpython/python_evaluator.h>
#include <libasr/codegen/evaluator.h>
#include <libasr/pass/do_loops.h>
#include <libasr/pass/for_all.h>
#include <libasr/pass/global_stmts.h>
#include <libasr/pass/implied_do_loops.h>
#include <libasr/pass/array_op.h>
#include <libasr/pass/class_constructor.h>
#include <libasr/pass/arr_slice.h>
#include <libasr/pass/print_arr.h>
#include <libasr/pass/unused_functions.h>
#include <libasr/pass/inline_function_calls.h>
#include <libasr/asr_utils.h>
#include <libasr/asr_verify.h>
#include <libasr/modfile.h>
#include <libasr/config.h>
#include <libasr/string_utils.h>
#include <lpython/utils.h>
#include <lpython/python_serialization.h>
#include <lpython/parser/tokenizer.h>
#include <lpython/parser/parser.h>

#include <cpp-terminal/terminal.h>
#include <cpp-terminal/prompt0.h>

namespace {

using LFortran::endswith;
using LFortran::CompilerOptions;
using LFortran::parse_python_file;

enum Backend {
    llvm, cpp, x86
};

enum ASRPass {
    do_loops, global_stmts, implied_do_loops, array_op,
    arr_slice, print_arr, class_constructor, unused_functions,
    inline_function_calls
};

std::string remove_extension(const std::string& filename) {
    size_t lastdot = filename.find_last_of(".");
    if (lastdot == std::string::npos) return filename;
    return filename.substr(0, lastdot);
}

std::string remove_path(const std::string& filename) {
    size_t lastslash = filename.find_last_of("/");
    if (lastslash == std::string::npos) return filename;
    return filename.substr(lastslash+1);
}

std::string get_kokkos_dir()
{
    char *env_p = std::getenv("LFORTRAN_KOKKOS_DIR");
    if (env_p) return env_p;
    std::cerr << "The code C++ generated by the C++ LFortran backend uses the Kokkos library" << std::endl;
    std::cerr << "(https://github.com/kokkos/kokkos). Please define the LFORTRAN_KOKKOS_DIR" << std::endl;
    std::cerr << "environment variable to point to the Kokkos installation." << std::endl;
    throw LFortran::LFortranException("LFORTRAN_KOKKOS_DIR is not defined");
}

#ifdef HAVE_LFORTRAN_LLVM

#endif

int emit_tokens(const std::string &infile, bool line_numbers, const CompilerOptions &compiler_options)
{
    std::string input = LFortran::read_file(infile);
    // Src -> Tokens
    Allocator al(64*1024*1024);
    std::vector<int> toks;
    std::vector<LFortran::YYSTYPE> stypes;
    std::vector<LFortran::Location> locations;
    LFortran::diag::Diagnostics diagnostics;
    auto res = LFortran::tokens(al, input, diagnostics, &stypes, &locations);
    LFortran::LocationManager lm;
    lm.in_filename = infile;
    lm.init_simple(input);
    std::cerr << diagnostics.render(input, lm, compiler_options);
    if (res.ok) {
        toks = res.result;
    } else {
        LFORTRAN_ASSERT(diagnostics.has_error())
        return 1;
    }

    for (size_t i=0; i < toks.size(); i++) {
        std::cout << LFortran::pickle_token(toks[i], stypes[i]);
        if (line_numbers) {
            std::cout << " " << locations[i].first << ":" << locations[i].last;
        }
        std::cout << std::endl;
    }
    return 0;
}


int emit_ast(const std::string &infile,
    const std::string &runtime_library_dir,
    CompilerOptions &compiler_options)
{
    Allocator al(4*1024);
    LFortran::diag::Diagnostics diagnostics;
    LFortran::Result<LFortran::LPython::AST::ast_t*> r = parse_python_file(
        al, runtime_library_dir, infile, diagnostics, compiler_options.new_parser);
    if (diagnostics.diagnostics.size() > 0) {
        LFortran::LocationManager lm;
        lm.in_filename = infile;
        // TODO: only read this once, and pass it as an argument to parse_python_file()
        std::string input = LFortran::read_file(infile);
        lm.init_simple(input);
        std::cerr << diagnostics.render(input, lm, compiler_options);
    }
    if (!r.ok) {
        LFORTRAN_ASSERT(diagnostics.has_error())
        return 1;
    }
    LFortran::LPython::AST::ast_t* ast = r.result;

    if (compiler_options.tree) {
        std::cout << LFortran::LPython::pickle_tree_python(*ast,
            compiler_options.use_colors) << std::endl;
    } else {
        std::cout << LFortran::LPython::pickle_python(*ast,
            compiler_options.use_colors, compiler_options.indent) << std::endl;
    }
    return 0;
}

int emit_asr(const std::string &infile,
    const std::string &runtime_library_dir,
    bool with_intrinsic_modules, CompilerOptions &compiler_options)
{
    Allocator al(4*1024);
    LFortran::diag::Diagnostics diagnostics;
    LFortran::LocationManager lm;
    lm.in_filename = infile;
    std::string input = LFortran::read_file(infile);
    lm.init_simple(input);
    LFortran::Result<LFortran::LPython::AST::ast_t*> r1 = parse_python_file(
        al, runtime_library_dir, infile, diagnostics, compiler_options.new_parser);
    std::cerr << diagnostics.render(input, lm, compiler_options);
    if (!r1.ok) {
        return 1;
    }
    LFortran::LPython::AST::ast_t* ast = r1.result;

    diagnostics.diagnostics.clear();
    LFortran::Result<LFortran::ASR::TranslationUnit_t*>
        r = LFortran::LPython::python_ast_to_asr(al, *ast, diagnostics, true,
            compiler_options.symtab_only);
    std::cerr << diagnostics.render(input, lm, compiler_options);
    if (!r.ok) {
        LFORTRAN_ASSERT(diagnostics.has_error())
        return 2;
    }
    LFortran::ASR::TranslationUnit_t* asr = r.result;

    if (compiler_options.tree) {
        std::cout << LFortran::pickle_tree(*asr, compiler_options.use_colors,
            with_intrinsic_modules) << std::endl;
    } else {
        std::cout << LFortran::pickle(*asr, compiler_options.use_colors, compiler_options.indent,
            with_intrinsic_modules) << std::endl;
    }
    return 0;
}

int emit_cpp(const std::string &infile,
    const std::string &runtime_library_dir,
    CompilerOptions &compiler_options)
{
    Allocator al(4*1024);
    LFortran::diag::Diagnostics diagnostics;
    LFortran::LocationManager lm;
    lm.in_filename = infile;
    std::string input = LFortran::read_file(infile);
    lm.init_simple(input);
    LFortran::Result<LFortran::LPython::AST::ast_t*> r = parse_python_file(
        al, runtime_library_dir, infile, diagnostics, compiler_options.new_parser);
    std::cerr << diagnostics.render(input, lm, compiler_options);
    if (!r.ok) {
        return 1;
    }
    LFortran::LPython::AST::ast_t* ast = r.result;

    diagnostics.diagnostics.clear();
    LFortran::Result<LFortran::ASR::TranslationUnit_t*>
        r1 = LFortran::LPython::python_ast_to_asr(al, *ast, diagnostics, true,
            compiler_options.symtab_only);
    std::cerr << diagnostics.render(input, lm, compiler_options);
    if (!r1.ok) {
        LFORTRAN_ASSERT(diagnostics.has_error())
        return 2;
    }
    LFortran::ASR::TranslationUnit_t* asr = r1.result;

    diagnostics.diagnostics.clear();
    auto res = LFortran::asr_to_cpp(al, *asr, diagnostics);
    std::cerr << diagnostics.render(input, lm, compiler_options);
    if (!res.ok) {
        LFORTRAN_ASSERT(diagnostics.has_error())
        return 3;
    }
    std::cout << res.result;
    return 0;
}

#ifdef HAVE_LFORTRAN_LLVM

int emit_llvm(const std::string &infile,
    const std::string &runtime_library_dir,
    CompilerOptions &compiler_options)
{
    Allocator al(4*1024);
    LFortran::diag::Diagnostics diagnostics;
    LFortran::LocationManager lm;
    lm.in_filename = infile;
    std::string input = LFortran::read_file(infile);
    lm.init_simple(input);
    LFortran::Result<LFortran::LPython::AST::ast_t*> r = parse_python_file(
        al, runtime_library_dir, infile, diagnostics, compiler_options.new_parser);
    std::cerr << diagnostics.render(input, lm, compiler_options);
    if (!r.ok) {
        return 1;
    }

    // Src -> AST -> ASR
    LFortran::LPython::AST::ast_t* ast = r.result;
    diagnostics.diagnostics.clear();
    LFortran::Result<LFortran::ASR::TranslationUnit_t*>
        r1 = LFortran::LPython::python_ast_to_asr(al, *ast, diagnostics, true,
            compiler_options.symtab_only);
    std::cerr << diagnostics.render(input, lm, compiler_options);
    if (!r1.ok) {
        LFORTRAN_ASSERT(diagnostics.has_error())
        return 2;
    }
    LFortran::ASR::TranslationUnit_t* asr = r1.result;
    diagnostics.diagnostics.clear();

    // ASR -> LLVM
    LFortran::PythonCompiler fe(compiler_options);
    LFortran::Result<std::unique_ptr<LFortran::LLVMModule>>
        res = fe.get_llvm3(*asr, diagnostics);
    std::cerr << diagnostics.render(input, lm, compiler_options);
    if (!res.ok) {
        LFORTRAN_ASSERT(diagnostics.has_error())
        return 3;
    }
    std::cout << (res.result)->str();
    return 0;
}

int compile_python_to_object_file(
        const std::string &infile,
        const std::string &outfile,
        const std::string &runtime_library_dir,
        CompilerOptions &compiler_options)
{
    Allocator al(4*1024);
    LFortran::diag::Diagnostics diagnostics;
    LFortran::LocationManager lm;
    lm.in_filename = infile;
    std::string input = LFortran::read_file(infile);
    lm.init_simple(input);
    LFortran::Result<LFortran::LPython::AST::ast_t*> r = parse_python_file(
        al, runtime_library_dir, infile, diagnostics, compiler_options.new_parser);
    std::cerr << diagnostics.render(input, lm, compiler_options);
    if (!r.ok) {
        return 1;
    }

    // Src -> AST -> ASR
    LFortran::LPython::AST::ast_t* ast = r.result;
    diagnostics.diagnostics.clear();
    LFortran::Result<LFortran::ASR::TranslationUnit_t*>
        r1 = LFortran::LPython::python_ast_to_asr(al, *ast, diagnostics, true,
            compiler_options.symtab_only);
    std::cerr << diagnostics.render(input, lm, compiler_options);
    if (!r1.ok) {
        LFORTRAN_ASSERT(diagnostics.has_error())
        return 2;
    }
    LFortran::ASR::TranslationUnit_t* asr = r1.result;
    diagnostics.diagnostics.clear();

    // ASR -> LLVM
    LFortran::PythonCompiler fe(compiler_options);
    LFortran::LLVMEvaluator e(compiler_options.target);
    std::unique_ptr<LFortran::LLVMModule> m;
    LFortran::Result<std::unique_ptr<LFortran::LLVMModule>>
        res = fe.get_llvm3(*asr, diagnostics);
    std::cerr << diagnostics.render(input, lm, compiler_options);
    if (!res.ok) {
        LFORTRAN_ASSERT(diagnostics.has_error())
        return 3;
    }
    m = std::move(res.result);
    e.save_object_file(*(m->m_m), outfile);
    return 0;
}

#endif


// infile is an object file
// outfile will become the executable
int link_executable(const std::vector<std::string> &infiles,
    const std::string &outfile,
    const std::string &runtime_library_dir, Backend backend,
    bool static_executable, bool kokkos,
    CompilerOptions &compiler_options)
{
    /*
    The `gcc` line for dynamic linking that is constructed below:

    gcc -o $outfile $infile \
        -Lsrc/runtime -Wl,-rpath=src/runtime -llpython_runtime

    is equivalent to the following:

    ld -o $outfile $infile \
        -Lsrc/runtime -rpath=src/runtime -llpython_runtime \
        -dynamic-linker /lib64/ld-linux-x86-64.so.2  \
        /usr/lib/x86_64-linux-gnu/Scrt1.o /usr/lib/x86_64-linux-gnu/libc.so

    and this for static linking:

    gcc -static -o $outfile $infile \
        -Lsrc/runtime -Wl,-rpath=src/runtime -llpython_runtime_static

    is equivalent to:

    ld -o $outfile $infile \
        -Lsrc/runtime -rpath=src/runtime -llpython_runtime_static \
        /usr/lib/x86_64-linux-gnu/crt1.o /usr/lib/x86_64-linux-gnu/crti.o \
        /usr/lib/x86_64-linux-gnu/libc.a \
        /usr/lib/gcc/x86_64-linux-gnu/7/libgcc_eh.a \
        /usr/lib/x86_64-linux-gnu/libc.a \
        /usr/lib/gcc/x86_64-linux-gnu/7/libgcc.a \
        /usr/lib/x86_64-linux-gnu/crtn.o

    This was tested on Ubuntu 18.04.

    The `gcc` and `ld` approaches are equivalent except:

    1. The `gcc` command knows how to find and link the `libc` library,
       while in `ld` we must do that manually
    2. For dynamic linking, we must also specify the dynamic linker for `ld`

    Notes:

    * We can use `lld` to do the linking via the `ld` approach, so `ld` is
      preferable if we can mitigate the issues 1. and 2.
    * If we ship our own libc (such as musl), then we know how to find it
      and link it, which mitigates the issue 1.
    * If we link `musl` statically, then issue 2. does not apply.
    * If we link `musl` dynamically, then we have to find the dynamic
      linker (doable), which mitigates the issue 2.

    One way to find the default dynamic linker is by:

        $ readelf -e /bin/bash | grep ld-linux
            [Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]

    There are probably simpler ways.
    */

#ifdef HAVE_LFORTRAN_LLVM
    std::string t = (compiler_options.target == "") ? LFortran::LLVMEvaluator::get_default_target_triple() : compiler_options.target;
#else
    std::string t = (compiler_options.platform == LFortran::Platform::Windows) ? "x86_64-pc-windows-msvc" : compiler_options.target;
#endif

    if (backend == Backend::llvm) {
        if (t == "x86_64-pc-windows-msvc") {
            std::string cmd = "link /NOLOGO /OUT:" + outfile + " ";
            for (auto &s : infiles) {
                cmd += s + " ";
            }
            cmd += runtime_library_dir + "\\lpython_runtime_static.lib";
            int err = system(cmd.c_str());
            if (err) {
                std::cout << "The command '" + cmd + "' failed." << std::endl;
                return 10;
            }
        } else {
            std::string CC = "cc";
            char *env_CC = std::getenv("LFORTRAN_CC");
            if (env_CC) CC = env_CC;
            std::string base_path = "\"" + runtime_library_dir + "\"";
            std::string options;
            std::string runtime_lib = "lpython_runtime";
            if (static_executable) {
                if (compiler_options.platform != LFortran::Platform::macOS_Intel
                && compiler_options.platform != LFortran::Platform::macOS_ARM) {
                    options += " -static ";
                }
                runtime_lib = "lpython_runtime_static";
            }
            std::string cmd = CC + options + " -o " + outfile + " ";
            for (auto &s : infiles) {
                cmd += s + " ";
            }
            cmd += + " -L"
                + base_path + " -Wl,-rpath," + base_path + " -l" + runtime_lib + " -lm";
            int err = system(cmd.c_str());
            if (err) {
                std::cout << "The command '" + cmd + "' failed." << std::endl;
                return 10;
            }
        }
        return 0;
    } else if (backend == Backend::cpp) {
        std::string CXX = "g++";
        std::string options, post_options;
        if (static_executable) {
            options += " -static ";
        }
        if (compiler_options.openmp) {
            options += " -fopenmp ";
        }
        if (kokkos) {
            std::string kokkos_dir = get_kokkos_dir();
            post_options += kokkos_dir + "/lib/libkokkoscontainers.a "
                + kokkos_dir + "/lib/libkokkoscore.a -ldl";
        }
        std::string cmd = CXX + options + " -o " + outfile + " ";
        for (auto &s : infiles) {
            cmd += s + " ";
        }
        cmd += + " -L";
        cmd += " " + post_options + " -lm";
        int err = system(cmd.c_str());
        if (err) {
            std::cout << "The command '" + cmd + "' failed." << std::endl;
            return 10;
        }
        return 0;
    } else if (backend == Backend::x86) {
        std::string cmd = "cp " + infiles[0] + " " + outfile;
        int err = system(cmd.c_str());
        if (err) {
            std::cout << "The command '" + cmd + "' failed." << std::endl;
            return 10;
        }
        return 0;
    } else {
        LFORTRAN_ASSERT(false);
        return 1;
    }
}

// int emit_c_preprocessor(const std::string &infile, CompilerOptions &compiler_options)
// {
//     std::string input = read_file(infile);
//
//     LFortran::CPreprocessor cpp(compiler_options);
//     LFortran::LocationManager lm;
//     lm.in_filename = infile;
//     std::string s = cpp.run(input, lm, cpp.macro_definitions);
//     std::cout << s;
//     return 0;
// }

} // anonymous namespace

int main(int argc, char *argv[])
{
    LFortran::initialize();
#if defined(HAVE_LFORTRAN_STACKTRACE)
    LFortran::print_stack_on_segfault();
#endif
    try {
        int dirname_length;
        LFortran::get_executable_path(LFortran::binary_executable_path, dirname_length);

        std::string runtime_library_dir = LFortran::get_runtime_library_dir();
        std::string rtlib_header_dir = LFortran::get_runtime_library_header_dir();
        Backend backend;

        bool arg_S = false;
        bool arg_c = false;
        bool arg_v = false;
        // bool arg_E = false;
        // bool arg_g = false;
        // std::string arg_J;
        // std::vector<std::string> arg_I;
        // std::vector<std::string> arg_l;
        // std::vector<std::string> arg_L;
        std::string arg_o;
        std::vector<std::string> arg_files;
        bool arg_version = false;
        bool show_tokens = false;
        bool show_ast = false;
        bool show_asr = false;
        bool show_cpp = false;
        bool with_intrinsic_modules = false;
        std::string arg_pass;
        bool arg_no_color = false;
        bool show_llvm = false;
        bool show_asm = false;
        bool time_report = false;
        bool static_link = false;
        std::string arg_backend = "llvm";
        std::string arg_kernel_f;
        bool print_targets = false;

        std::string arg_fmt_file;
        // int arg_fmt_indent = 4;
        // bool arg_fmt_indent_unit = false;
        // bool arg_fmt_inplace = false;
        // bool arg_fmt_no_color = false;

        std::string arg_mod_file;
        // bool arg_mod_show_asr = false;
        // bool arg_mod_no_color = false;

        std::string arg_pywrap_file;
        std::string arg_pywrap_array_order="f";

        CompilerOptions compiler_options;

        CLI::App app{"LPython: modern interactive LLVM-based Python compiler"};
        // Standard options compatible with gfortran, gcc or clang
        // We follow the established conventions
        app.add_option("files", arg_files, "Source files");
        // Should the following Options required for LPython??
        // Instead we need support all the options from Python 3
        app.add_flag("-S", arg_S, "Emit assembly, do not assemble or link");
        app.add_flag("-c", arg_c, "Compile and assemble, do not link");
        app.add_option("-o", arg_o, "Specify the file to place the output into");
        app.add_flag("-v", arg_v, "Be more verbose");
        // app.add_flag("-E", arg_E, "Preprocess only; do not compile, assemble or link");
        // app.add_option("-l", arg_l, "Link library option");
        // app.add_option("-L", arg_L, "Library path option");
        // app.add_option("-I", arg_I, "Include path")->allow_extra_args(false);
        // app.add_option("-J", arg_J, "Where to save mod files");
        // app.add_flag("-g", arg_g, "Compile with debugging information");
        // app.add_option("-D", compiler_options.c_preprocessor_defines, "Define <macro>=<value> (or 1 if <value> omitted)")->allow_extra_args(false);
        app.add_flag("--version", arg_version, "Display compiler version information");

        // LPython specific options
        app.add_flag("--cpp", compiler_options.c_preprocessor, "Enable C preprocessing");
        app.add_flag("--show-tokens", show_tokens, "Show tokens for the given python file and exit");
        app.add_flag("--new-parser", compiler_options.new_parser, "Use lpython parser");
        app.add_flag("--show-ast", show_ast, "Show AST for the given python file and exit");
        app.add_flag("--show-asr", show_asr, "Show ASR for the given python file and exit");
        app.add_flag("--show-llvm", show_llvm, "Show LLVM IR for the given file and exit");
        app.add_flag("--show-cpp", show_cpp, "Show C++ translation source for the given python file and exit");
        app.add_flag("--show-asm", show_asm, "Show assembly for the given file and exit");
        app.add_flag("--show-stacktrace", compiler_options.show_stacktrace, "Show internal stacktrace on compiler errors");
        app.add_flag("--with-intrinsic-mods", with_intrinsic_modules, "Show intrinsic modules in ASR");
        app.add_flag("--no-color", arg_no_color, "Turn off colored AST/ASR");
        app.add_flag("--indent", compiler_options.indent, "Indented print ASR/AST");
        app.add_flag("--tree", compiler_options.tree, "Tree structure print ASR/AST");
        app.add_option("--pass", arg_pass, "Apply the ASR pass and show ASR (implies --show-asr)");
        app.add_flag("--symtab-only", compiler_options.symtab_only, "Only create symbol tables in ASR (skip executable stmt)");
        app.add_flag("--time-report", time_report, "Show compilation time report");
        app.add_flag("--static", static_link, "Create a static executable");
        app.add_flag("--no-warnings", compiler_options.no_warnings, "Turn off all warnings");
        app.add_flag("--no-error-banner", compiler_options.no_error_banner, "Turn off error banner");
        app.add_option("--backend", arg_backend, "Select a backend (llvm, cpp, x86)")->capture_default_str();
        app.add_flag("--openmp", compiler_options.openmp, "Enable openmp");
        app.add_flag("--fast", compiler_options.fast, "Best performance (disable strict standard compliance)");
        app.add_option("--target", compiler_options.target, "Generate code for the given target")->capture_default_str();
        app.add_flag("--print-targets", print_targets, "Print the registered targets");

        /*
        * Subcommands:
        */

        // fmt: Should LPython support `fmt` subcommand??
        // CLI::App &fmt = *app.add_subcommand("fmt", "Format Fortran source files.");
        // fmt.add_option("file", arg_fmt_file, "Fortran source file to format")->required();
        // fmt.add_flag("-i", arg_fmt_inplace, "Modify <file> in-place (instead of writing to stdout)");
        // fmt.add_option("--spaces", arg_fmt_indent, "Number of spaces to use for indentation")->capture_default_str();
        // fmt.add_flag("--indent-unit", arg_fmt_indent_unit, "Indent contents of sub / fn / prog / mod");
        // fmt.add_flag("--no-color", arg_fmt_no_color, "Turn off color when writing to stdout");

        // kernel
        CLI::App &kernel = *app.add_subcommand("kernel", "Run in Jupyter kernel mode.");
        kernel.add_option("-f", arg_kernel_f, "The kernel connection file")->required();

        // mod
        // CLI::App &mod = *app.add_subcommand("mod", "Fortran mod file utilities.");
        // mod.add_option("file", arg_mod_file, "Mod file (*.mod)")->required();
        // mod.add_flag("--show-asr", arg_mod_show_asr, "Show ASR for the module");
        // mod.add_flag("--no-color", arg_mod_no_color, "Turn off colored ASR");

        // pywrap
        CLI::App &pywrap = *app.add_subcommand("pywrap", "Python wrapper generator");
        pywrap.add_option("file", arg_pywrap_file, "Fortran source file (*.f90)")->required();
        pywrap.add_option("--array-order", arg_pywrap_array_order,
                "Select array order (c, f)")->capture_default_str();


        app.get_formatter()->column_width(25);
        app.require_subcommand(0, 1);
        CLI11_PARSE(app, argc, argv);

        if (arg_version) {
            std::string version = LFORTRAN_VERSION;
            std::cout << "LPython version: " << version << std::endl;
            std::cout << "Platform: ";
            switch (compiler_options.platform) {
                case (LFortran::Platform::Linux) : std::cout << "Linux"; break;
                case (LFortran::Platform::macOS_Intel) : std::cout << "macOS Intel"; break;
                case (LFortran::Platform::macOS_ARM) : std::cout << "macOS ARM"; break;
                case (LFortran::Platform::Windows) : std::cout << "Windows"; break;
                case (LFortran::Platform::FreeBSD) : std::cout << "FreeBSD"; break;
            }
            std::cout << std::endl;
#ifdef HAVE_LFORTRAN_LLVM
            std::cout << "Default target: " << LFortran::LLVMEvaluator::get_default_target_triple() << std::endl;
#endif
            return 0;
        }

        if (print_targets) {
#ifdef HAVE_LFORTRAN_LLVM
            LFortran::LLVMEvaluator::print_targets();
            return 0;
#else
            std::cerr << "The --print-targets option requires the LLVM backend to be enabled. Recompile with `WITH_LLVM=yes`." << std::endl;
            return 1;
#endif
        }

        compiler_options.use_colors = !arg_no_color;

        // if (fmt) {
        //     return format(arg_fmt_file, arg_fmt_inplace, !arg_fmt_no_color,
        //         arg_fmt_indent, arg_fmt_indent_unit, compiler_options);
        // }

        if (kernel) {
            std::cerr << "The kernel subcommand is not implemented yet for LPython." << std::endl;
            return 1;
        }

        // if (mod) {
        //     if (arg_mod_show_asr) {
        //         Allocator al(1024*1024);
        //         LFortran::ASR::TranslationUnit_t *asr;
        //         asr = LFortran::mod_to_asr(al, arg_mod_file);
        //         std::cout << LFortran::pickle(*asr, !arg_mod_no_color) << std::endl;
        //         return 0;
        //     }
        //     return 0;
        // }

        if (pywrap) {
            std::cerr << "Pywrap is not implemented yet." << std::endl;
            return 1;
        }

        if (arg_backend == "llvm") {
            backend = Backend::llvm;
        } else if (arg_backend == "cpp") {
            backend = Backend::cpp;
        } else if (arg_backend == "x86") {
            backend = Backend::x86;
        } else {
            std::cerr << "The backend must be one of: llvm, cpp, x86." << std::endl;
            return 1;
        }

        if (arg_files.size() == 0) {
            std::cerr << "Interactive prompt is not implemented yet in LPython" << std::endl;
            return 1;
        }

        // TODO: for now we ignore the other filenames, only handle
        // the first:
        std::string arg_file = arg_files[0];

        std::string outfile;
        std::string basename;
        basename = remove_extension(arg_file);
        basename = remove_path(basename);
        if (arg_o.size() > 0) {
            outfile = arg_o;
        } else if (arg_S) {
            outfile = basename + ".s";
        } else if (arg_c) {
            outfile = basename + ".o";
        } else if (show_tokens) {
            outfile = basename + ".tok";
        } else if (show_ast) {
            outfile = basename + ".ast";
        } else if (show_asr) {
            outfile = basename + ".asr";
        } else if (show_llvm) {
            outfile = basename + ".ll";
        } else {
            outfile = "a.out";
        }

        // if (arg_E) {
        //     return emit_c_preprocessor(arg_file, compiler_options);
        // }

        std::vector<ASRPass> passes;
        if (arg_pass != "") {
            if (arg_pass == "do_loops") {
                passes.push_back(ASRPass::do_loops);
            } else if (arg_pass == "global_stmts") {
                passes.push_back(ASRPass::global_stmts);
            } else if (arg_pass == "implied_do_loops") {
                passes.push_back(ASRPass::implied_do_loops);
            } else if (arg_pass == "array_op") {
                passes.push_back(ASRPass::array_op);
            } else if (arg_pass == "inline_function_calls") {
                passes.push_back(ASRPass::inline_function_calls);
            } else if (arg_pass == "class_constructor") {
                passes.push_back(ASRPass::class_constructor);
            } else if (arg_pass == "print_arr") {
                passes.push_back(ASRPass::print_arr);
            } else if (arg_pass == "arr_slice") {
                passes.push_back(ASRPass::arr_slice);
            } else if (arg_pass == "unused_functions") {
                passes.push_back(ASRPass::unused_functions);
            } else {
                std::cerr << "Pass must be one of: do_loops, global_stmts, implied_do_loops, array_op, class_constructor, print_arr, arr_slice, unused_functions" << std::endl;
                return 1;
            }
            show_asr = true;
        }
        if (show_tokens) {
            return emit_tokens(arg_file, true, compiler_options);
        }
        if (show_ast) {
            return emit_ast(arg_file, runtime_library_dir, compiler_options);
        }
        if (show_asr) {
            return emit_asr(arg_file, runtime_library_dir,
                    with_intrinsic_modules, compiler_options);
        }
        if (show_cpp) {
            return emit_cpp(arg_file, runtime_library_dir, compiler_options);
        }
        if (show_llvm) {
#ifdef HAVE_LFORTRAN_LLVM
            return emit_llvm(arg_file, runtime_library_dir, compiler_options);
#else
            std::cerr << "The --show-llvm option requires the LLVM backend to be enabled. Recompile with `WITH_LLVM=yes`." << std::endl;
            return 1;
#endif
        }
        if (show_asm) {
            std::cerr << "The --show-asm option is not implemented yet." << std::endl;
            return 1;
        }
        if (arg_S) {
            if (backend == Backend::llvm) {
                std::cerr << "The -S option is not implemented yet for the LLVM backend." << std::endl;
                return 1;
            } else if (backend == Backend::cpp) {
                std::cerr << "The C++ backend does not work with the -S option yet." << std::endl;
                return 1;
            } else {
                LFORTRAN_ASSERT(false);
            }
        }

        if (arg_c) {
            if (backend == Backend::llvm) {
#ifdef HAVE_LFORTRAN_LLVM
                return compile_python_to_object_file(arg_file, outfile, runtime_library_dir, compiler_options);
#else
                std::cerr << "The -c option requires the LLVM backend to be enabled. Recompile with `WITH_LLVM=yes`." << std::endl;
                return 1;
#endif
            } else {
                throw LFortran::LFortranException("Unsupported backend.");
            }
        }

        if (endswith(arg_file, ".py"))
        {
            std::string tmp_o = outfile + ".tmp.o";
            int err;
            if (backend == Backend::llvm) {
#ifdef HAVE_LFORTRAN_LLVM
                err = compile_python_to_object_file(arg_file, tmp_o, runtime_library_dir, compiler_options);
#else
                std::cerr << "Compiling Python files to object files requires the LLVM backend to be enabled. Recompile with `WITH_LLVM=yes`." << std::endl;
                return 1;
#endif
            } else {
                throw LFortran::LFortranException("Unsupported backend.");
            }
            if (err) return err;
            return link_executable({tmp_o}, outfile, runtime_library_dir,
                    backend, static_link, true, compiler_options);
        } else {
            return link_executable(arg_files, outfile, runtime_library_dir,
                    backend, static_link, true, compiler_options);
        }
    } catch(const LFortran::LFortranException &e) {
        std::cerr << "Internal Compiler Error: Unhandled exception" << std::endl;
        std::vector<LFortran::StacktraceItem> d = e.stacktrace_addresses();
        get_local_addresses(d);
        get_local_info(d);
        std::cerr << stacktrace2str(d, LFortran::stacktrace_depth);
        std::cerr << e.name() + ": " << e.msg() << std::endl;
        return 1;
    } catch(const std::runtime_error &e) {
        std::cerr << "runtime_error: " << e.what() << std::endl;
        return 1;
    } catch(const std::exception &e) {
        std::cerr << "std::exception: " << e.what() << std::endl;
        return 1;
    } catch(...) {
        std::cerr << "Unknown Exception" << std::endl;
        return 1;
    }
    return 0;
}
