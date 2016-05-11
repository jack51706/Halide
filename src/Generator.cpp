#include "Generator.h"
#include "Output.h"
#include "Util.h"

namespace {

// Return true iff the name is valid for Generators or Params.
// (NOTE: gcc didn't add proper std::regex support until v4.9;
// we don't yet require this, hence the hand-rolled replacement.)

bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// Note that this includes '_'
bool is_alnum(char c) { return is_alpha(c) || (c == '_') || (c >= '0' && c <= '9'); }

// Basically, a valid C identifier, except:
//
// -- initial _ is forbidden (rather than merely "reserved")
// -- two underscores in a row is also forbidden
bool is_valid_name(const std::string& n) {
    if (n.empty()) return false;
    if (!is_alpha(n[0])) return false;
    for (size_t i = 1; i < n.size(); ++i) {
        if (!is_alnum(n[i])) return false;
        if (n[i] == '_' && n[i-1] == '_') return false;
    }
    return true;
}

}  // namespace

namespace Halide {
namespace Internal {

const std::map<std::string, Halide::Type> &get_halide_type_enum_map() {
    static const std::map<std::string, Halide::Type> halide_type_enum_map{
        {"bool", Halide::Bool()},
        {"int8", Halide::Int(8)},
        {"int16", Halide::Int(16)},
        {"int32", Halide::Int(32)},
        {"uint8", Halide::UInt(8)},
        {"uint16", Halide::UInt(16)},
        {"uint32", Halide::UInt(32)},
        {"float32", Halide::Float(32)},
        {"float64", Halide::Float(64)}
    };
    return halide_type_enum_map;
}

int generate_filter_main(int argc, char **argv, std::ostream &cerr) {
    const char kUsage[] = "gengen [-g GENERATOR_NAME] [-f FUNCTION_NAME] [-o OUTPUT_DIR] [-r RUNTIME_NAME] [-e EMIT_OPTIONS] [-x EXTENSION_OPTIONS] [-n FILE_BASE_NAME] "
                          "target=target-string [generator_arg=value [...]]\n\n"
                          "  -e  A comma separated list of files to emit. Accepted values are "
                          "[o, h, assembly, bitcode, stmt, html, cpp]. If omitted, default value is [o, h].\n"
                          "  -x  A comma separated list of file extension pairs to substitute during file naming, "
                          "in the form [.old=.new[,.old2=.new2]]\n";

    std::map<std::string, std::string> flags_info = { { "-f", "" },
                                                      { "-g", "" },
                                                      { "-o", "" },
                                                      { "-e", "" },
                                                      { "-n", "" },
                                                      { "-x", "" },
                                                      { "-r", "" }};
    std::map<std::string, std::string> generator_args;

    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') {
            std::vector<std::string> v = split_string(argv[i], "=");
            if (v.size() != 2 || v[0].empty() || v[1].empty()) {
                cerr << kUsage;
                return 1;
            }
            generator_args[v[0]] = v[1];
            continue;
        }
        auto it = flags_info.find(argv[i]);
        if (it != flags_info.end()) {
            if (i + 1 >= argc) {
                cerr << kUsage;
                return 1;
            }
            it->second = argv[i + 1];
            ++i;
            continue;
        }
        cerr << "Unknown flag: " << argv[i] << "\n";
        cerr << kUsage;
        return 1;
    }

    std::string runtime_name = flags_info["-r"];

    std::vector<std::string> generator_names = GeneratorRegistry::enumerate();
    if (generator_names.size() == 0 && runtime_name.empty()) {
        cerr << "No generators have been registered and not compiling a standalone runtime\n";
        cerr << kUsage;
        return 1;
    }

    std::string generator_name = flags_info["-g"];
    if (generator_name.empty() && runtime_name.empty()) {
        // If -g isn't specified, but there's only one generator registered, just use that one.
        if (generator_names.size() > 1) {
            cerr << "-g must be specified if multiple generators are registered:\n";
            for (auto name : generator_names) {
                cerr << "    " << name << "\n";
            }
            cerr << kUsage;
            return 1;
        }
        generator_name = generator_names[0];
    }
    std::string function_name = flags_info["-f"];
    if (function_name.empty()) {
        // If -f isn't specified, assume function name = generator name.
        function_name = generator_name;
    }
    std::string output_dir = flags_info["-o"];
    if (output_dir.empty()) {
        cerr << "-o must always be specified.\n";
        cerr << kUsage;
        return 1;
    }
    if (generator_args.find("target") == generator_args.end()) {
        cerr << "Target missing\n";
        cerr << kUsage;
        return 1;
    }
    // it's OK for file_base_name to be empty: filename will be based on function name
    std::string file_base_name = flags_info["-n"];

    GeneratorBase::EmitOptions emit_options;
    // Ensure all flags start as false.
    emit_options.emit_o = emit_options.emit_h = false;

    std::vector<std::string> emit_flags = split_string(flags_info["-e"], ",");
    if (emit_flags.empty() || (emit_flags.size() == 1 && emit_flags[0].empty())) {
        // If omitted or empty, assume .o and .h
        emit_options.emit_o = emit_options.emit_h = true;
    } else {
        // If anything specified, only emit what is enumerated
        for (const std::string &opt : emit_flags) {
            if (opt == "assembly") {
                emit_options.emit_assembly = true;
            } else if (opt == "bitcode") {
                emit_options.emit_bitcode = true;
            } else if (opt == "stmt") {
                emit_options.emit_stmt = true;
            } else if (opt == "html") {
                emit_options.emit_stmt_html = true;
            } else if (opt == "cpp") {
                emit_options.emit_cpp = true;
            } else if (opt == "o") {
                emit_options.emit_o = true;
            } else if (opt == "h") {
                emit_options.emit_h = true;
            } else if (!opt.empty()) {
                cerr << "Unrecognized emit option: " << opt
                     << " not one of [assembly, bitcode, stmt, html], ignoring.\n";
            }
        }
    }

    auto extension_flags = split_string(flags_info["-x"], ",");
    for (const std::string &x : extension_flags) {
        if (x.empty()) {
            continue;
        }
        auto ext_pair = split_string(x, "=");
        if (ext_pair.size() != 2) {
            cerr << "Malformed -x option: " << x << "\n";
            cerr << kUsage;
            return 1;
        }
        emit_options.extensions[ext_pair[0]] = ext_pair[1];
    }

    auto target_string = generator_args["target"];
    if (!runtime_name.empty()) {
        compile_standalone_runtime(output_dir + "/" + runtime_name,
                                   parse_target_string(target_string));
        if (generator_name.empty()) {
            // We're just compiling a runtime
            return 0;
        }
    }

    auto target_strings = split_string(target_string, ",");
    if (target_strings.size() == 1) {
        auto gen = GeneratorRegistry::create(generator_name, generator_args);
        if (gen == nullptr) {
            cerr << "Unknown generator: " << generator_name << "\n";
            cerr << kUsage;
            return 1;
        }
        gen->emit_filter(output_dir, function_name, file_base_name, emit_options);
        return 0;
    } else {
        for (auto sub_target_string : target_strings) {
            auto sub_generator_args = generator_args;
            sub_generator_args["target"] = sub_target_string;
            auto gen = GeneratorRegistry::create(generator_name, sub_generator_args);
            if (gen == nullptr) {
                cerr << "Unknown generator: " << generator_name << "\n";
                cerr << kUsage;
                return 1;
            }
            gen->emit_filter(output_dir, function_name, file_base_name, emit_options);
        }
    }
    return 0;
}

GeneratorParamBase::GeneratorParamBase(const std::string &name) : name(name) {
    ObjectInstanceRegistry::register_instance(this, 0, ObjectInstanceRegistry::GeneratorParam,
                                              this, nullptr);
}

GeneratorParamBase::GeneratorParamBase(const GeneratorParamBase &that) : name(that.name) {
    ObjectInstanceRegistry::register_instance(this, 0, ObjectInstanceRegistry::GeneratorParam,
                                              this, nullptr);
}

GeneratorParamBase::~GeneratorParamBase() { ObjectInstanceRegistry::unregister_instance(this); }

/* static */
GeneratorRegistry &GeneratorRegistry::get_registry() {
    static GeneratorRegistry *registry = new GeneratorRegistry;
    return *registry;
}

/* static */
void GeneratorRegistry::register_factory(const std::string &name,
                                         std::unique_ptr<GeneratorFactory> factory) {
    user_assert(is_valid_name(name)) << "Invalid Generator name: " << name;
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    internal_assert(registry.factories.find(name) == registry.factories.end())
        << "Duplicate Generator name: " << name;
    registry.factories[name] = std::move(factory);
}

/* static */
void GeneratorRegistry::unregister_factory(const std::string &name) {
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    internal_assert(registry.factories.find(name) != registry.factories.end())
        << "Generator not found: " << name;
    registry.factories.erase(name);
}

/* static */
std::unique_ptr<GeneratorBase> GeneratorRegistry::create(const std::string &name,
                                                         const GeneratorParamValues &params) {
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto it = registry.factories.find(name);
    user_assert(it != registry.factories.end()) << "Generator not found: " << name;
    return it->second->create(params);
}

/* static */
std::vector<std::string> GeneratorRegistry::enumerate() {
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    std::vector<std::string> result;
    for (const auto& i : registry.factories) {
        result.push_back(i.first);
    }
    return result;
}

GeneratorBase::GeneratorBase(size_t size, const void *introspection_helper) : size(size), params_built(false) {
    ObjectInstanceRegistry::register_instance(this, size, ObjectInstanceRegistry::Generator, this, introspection_helper);
}

GeneratorBase::~GeneratorBase() { ObjectInstanceRegistry::unregister_instance(this); }

void GeneratorBase::rebuild_params() {
    params_built = false;
    filter_arguments.clear();
    generator_params.clear();
    build_params();
}

void GeneratorBase::build_params() {
    if (!params_built) {
        std::vector<void *> vf = ObjectInstanceRegistry::instances_in_range(
            this, size, ObjectInstanceRegistry::FilterParam);
        for (size_t i = 0; i < vf.size(); ++i) {
            Parameter *param = static_cast<Parameter *>(vf[i]);
            internal_assert(param != nullptr);
            user_assert(param->is_explicit_name()) << "Params in Generators must have explicit names: " << param->name();
            user_assert(is_valid_name(param->name())) << "Invalid Param name: " << param->name();
            for (const Argument& arg : filter_arguments) {
                user_assert(arg.name != param->name()) << "Duplicate Param name: " << param->name();
            }
            Expr def, min, max;
            if (!param->is_buffer()) {
                def = param->get_scalar_expr();
                min = param->get_min_value();
                max = param->get_max_value();
            }
            filter_arguments.push_back(Argument(param->name(),
                param->is_buffer() ? Argument::InputBuffer : Argument::InputScalar,
                param->type(), param->dimensions(), def, min, max));
        }

        std::vector<void *> vg = ObjectInstanceRegistry::instances_in_range(
            this, size, ObjectInstanceRegistry::GeneratorParam);
        for (size_t i = 0; i < vg.size(); ++i) {
            GeneratorParamBase *param = static_cast<GeneratorParamBase *>(vg[i]);
            internal_assert(param != nullptr);
            user_assert(is_valid_name(param->name)) << "Invalid GeneratorParam name: " << param->name;
            user_assert(generator_params.find(param->name) == generator_params.end())
                << "Duplicate GeneratorParam name: " << param->name;
            generator_params[param->name] = param;
        }
        params_built = true;
    }
}

GeneratorParamValues GeneratorBase::get_generator_param_values() {
    build_params();
    GeneratorParamValues results;
    for (auto key_value : generator_params) {
        GeneratorParamBase *param = key_value.second;
        results[param->name] = param->to_string();
    }
    return results;
}

void GeneratorBase::set_generator_param_values(const GeneratorParamValues &params) {
    build_params();
    for (auto key_value : params) {
        const std::string &key = key_value.first;
        const std::string &value = key_value.second;
        auto param = generator_params.find(key);
        user_assert(param != generator_params.end())
            << "Generator has no GeneratorParam named: " << key;
        param->second->from_string(value);
    }
}

std::vector<Argument> GeneratorBase::get_filter_output_types() {
    std::vector<Argument> output_types;
    Pipeline pipeline = build_pipeline();
    std::vector<Func> pipeline_results = pipeline.outputs();
    for (Func func : pipeline_results) {
        for (Halide::Type t : func.output_types()) {
            std::string name = "result_" + std::to_string(output_types.size());
            output_types.push_back(Halide::Argument(name, Halide::Argument::OutputBuffer, t, func.dimensions()));
        }
    }
    return output_types;
}

std::string get_extension(const std::string& def, const GeneratorBase::EmitOptions &options) {
    auto it = options.extensions.find(def);
    if (it != options.extensions.end()) {
        return it->second;
    }
    return def;
}

void GeneratorBase::emit_filter(const std::string &output_dir,
                                const std::string &function_name,
                                const std::string &file_base_name,
                                const EmitOptions &options) {
    build_params();

    Pipeline pipeline = build_pipeline();

    // Building the pipeline may mutate the params and imageparams.
    rebuild_params();

    std::vector<Halide::Argument> inputs = get_filter_arguments();

    std::vector<std::string> namespaces;
    std::string simple_name = extract_namespaces(function_name, namespaces);

    std::string base_path = output_dir + "/" + (file_base_name.empty() ? simple_name : file_base_name);

    Module m = pipeline.compile_to_module(inputs, function_name, target);

    Outputs output_files;
    if (options.emit_o) {
        // If the target arch is pnacl, then the output "object" file is
        // actually a pnacl bitcode file.
        if (Target(target).arch == Target::PNaCl) {
            output_files.object_name = base_path + get_extension(".bc", options);
        } else if (Target(target).os == Target::Windows &&
                   !Target(target).has_feature(Target::MinGW)) {
            // If it's windows, then we're emitting a COFF file
            output_files.object_name = base_path + get_extension(".obj", options);
        } else {
            // Otherwise it is an ELF or Mach-o
            output_files.object_name = base_path + get_extension(".o", options);
        }
    }
    if (options.emit_assembly) {
        output_files.assembly_name = base_path + get_extension(".s", options);
    }
    if (options.emit_bitcode) {
        // In this case, bitcode refers to the LLVM IR generated by Halide
        // and passed to LLVM, for both the pnacl and ordinary archs
        output_files.bitcode_name = base_path + get_extension(".bc", options);
    }
    if (options.emit_h) {
        output_files.c_header_name = base_path + get_extension(".h", options);
    }
    if (options.emit_cpp) {
        output_files.c_source_name = base_path + get_extension(".cpp", options);
    }
    if (options.emit_stmt) {
        output_files.stmt_name = base_path + get_extension(".stmt", options);
    }
    if (options.emit_stmt_html) {
        output_files.stmt_html_name = base_path + get_extension(".html", options);
    }
    compile_module_to_outputs(m, output_files);
}

Func GeneratorBase::call_extern(std::initializer_list<ExternFuncArgument> function_arguments,
                                std::string function_name){
    Pipeline p = build_pipeline();
    user_assert(p.outputs().size() == 1) \
        << "Can only call_extern Pipelines with a single output Func\n";
    Func f = p.outputs()[0];
    Func f_extern;
    if (function_name.empty()) {
        function_name = generator_name();
        user_assert(!function_name.empty())
            << "call_extern: generator_name is empty\n";
    }
    f_extern.define_extern(function_name, function_arguments, f.output_types(), f.dimensions());
    return f_extern;
}

Func GeneratorBase::call_extern_by_name(const std::string &generator_name,
                                        std::initializer_list<ExternFuncArgument> function_arguments,
                                        const std::string &function_name,
                                        const GeneratorParamValues &generator_params) {
    std::unique_ptr<GeneratorBase> extern_gen = GeneratorRegistry::create(generator_name, generator_params);
    user_assert(extern_gen != nullptr) << "Unknown generator: " << generator_name << "\n";
    // Note that the Generator's target is not set; at present, this shouldn't matter for
    // define_extern() functions, since none of the linkage should vary by Target.
    return extern_gen->call_extern(function_arguments, function_name);
}

}  // namespace Internal
}  // namespace Halide
