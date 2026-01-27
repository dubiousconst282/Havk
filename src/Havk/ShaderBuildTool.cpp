#include <cstdarg>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <slang.h>
#include <slang-com-ptr.h>

#include <Havx/SystemUtils.h>

static bool IsFileContentEquals(const std::filesystem::path& path, const uint8_t* data, size_t length) {
    auto fs = std::ifstream(path, std::ios::binary | std::ios::ate);
    if (!fs.good() || (size_t)fs.tellg() != length) return false;

    fs.seekg(0);
    char buffer[4096];

    for (size_t pos = 0; pos < length; pos += sizeof(buffer)) {
        size_t chunkLen = std::min(sizeof(buffer), length - pos);
        fs.read(buffer, chunkLen);

        if (!fs.good() || memcmp(data + pos, buffer, chunkLen) != 0) return false;
    }
    return true;
}
// Overwrite file iff contents are different. This avoids dirtying timestamps and triggering rebuilds.
static void UpdateFile(const std::filesystem::path& path, const void* data, size_t length) {
    if (IsFileContentEquals(path, (const uint8_t*)data, length)) return;

    std::filesystem::create_directories(path.parent_path());
    std::ofstream fs;
    fs.exceptions(std::ios::failbit);
    fs.open(path, std::ios::binary | std::ios::trunc);
    fs.write((char*)data, (std::streamsize)length);
}

static bool IsSubpath(const std::filesystem::path& path, const std::filesystem::path& base) {
    auto rel = std::filesystem::relative(path, base);
    return !rel.empty() && rel.native()[0] != '.';
}

static bool IsBuiltinDescriptorHandle(slang::TypeReflection* type) {
    const char* name = type->getName();
    return strcmp(name, "ImageHandle") == 0 || strcmp(name, "SamplerHandle") == 0 || strcmp(name, "AccelStructHandle") == 0;
}

struct TypeGraph {
    struct TypeInfo {
        slang::IModule* Module;
        std::string Namespace;
    };
    std::unordered_map<slang::TypeReflection*, TypeInfo> Entries;
    std::unordered_map<slang::FunctionReflection*, std::string> ParentNamespaces;

    std::string BaseNamespace;

    TypeInfo* GetInfo(slang::TypeReflection* type) {
        auto iter = Entries.find(type);
        return iter != Entries.end() ? &iter->second : nullptr;
    }

    void RegisterTypes(slang::IModule* module, slang::DeclReflection* entity, const std::string& parentNs = "") {
        std::string ns = parentNs;
        if (ns.empty() && !BaseNamespace.empty()) {
            ns = BaseNamespace;
        }
        if (entity->getKind() == slang::DeclReflection::Kind::Namespace) {
            if (!ns.empty()) ns += "::";
            ns += entity->getName();
        }

        for (uint32_t i = 0; i < entity->getChildrenCount(); i++) {
            slang::DeclReflection* child = entity->getChild(i);

            switch (child->getKind()) {
                case slang::DeclReflection::Kind::Struct: {
                    Entries.insert({ child->getType(), { .Module = module, .Namespace = ns } });
                    break;
                }
                case slang::DeclReflection::Kind::Func: {
                    ParentNamespaces.insert({ child->asFunction(), ns });
                    break;
                }
                case slang::DeclReflection::Kind::Namespace: {
                    RegisterTypes(module, child, ns);
                    break;
                }
                default: break;
            }
        }
    }

    void GetOrderedDependencies(slang::TypeReflection* type, std::vector<slang::TypeReflection*>& postOrder) {
        if (std::find(postOrder.begin(), postOrder.end(), type) != postOrder.end()) return;

        switch (type->getKind()) {
            case slang::TypeReflection::Kind::Struct: {
                for (uint32_t i = 0; i < type->getFieldCount(); i++) {
                    auto field = type->getFieldByIndex(i);
                    GetOrderedDependencies(field->getType(), postOrder);
                }
                postOrder.push_back(type);
                break;
            }
            case slang::TypeReflection::Kind::Pointer: {
                auto elemType = type->getGenericContainer()->getConcreteType(type->getGenericContainer()->getTypeParameter(0));
                GetOrderedDependencies(elemType, postOrder);
                break;
            }
            case slang::TypeReflection::Kind::Array:
                GetOrderedDependencies(type->getElementType(), postOrder);
                break;
            case slang::TypeReflection::Kind::Scalar:
            case slang::TypeReflection::Kind::Vector:
            case slang::TypeReflection::Kind::Matrix:
                return; // ignore primitive types
            default: {
                fprintf(stderr, "warn: Don't know how to process type '%s' kind=%d\n", type->getName(), type->getKind());
                return;
            }
        }
    }
};

struct CodePrinter {
    TypeGraph* Types;
    std::string Buffer;
    int IndentLevel = 0;

    std::string CurrentNamespace;

    CodePrinter(TypeGraph* tg) : Types(tg) { }

    void Indent() { Buffer.append(IndentLevel * 4, ' '); }

    void Begin(const char* fmt, const char* arg1 = nullptr, const char* arg2 = nullptr) {
        Indent();
        AppendFmt(fmt, arg1, arg2);
        IndentLevel++;
    }
    void End(const char* fmt, const char* arg1 = nullptr) {
        IndentLevel--;
        Indent();
        AppendFmt(fmt, arg1);
    }

    void SetNamespace(std::string_view ns) {
        if (ns.empty() && !CurrentNamespace.empty()) {
            End("}; // namespace %s\n", CurrentNamespace.data());
            CurrentNamespace = "";
        } else if (!ns.empty() && CurrentNamespace != ns) {
            Begin("namespace %s {\n", ns.data());
            CurrentNamespace = ns;
        }
    }

    void Append(std::string_view str) { Buffer.append(str); }
    void AppendFmt(const char* fmt, ...) {
        if (fmt[0] == '\t') {
            Indent();
            fmt++;
        }
        size_t currSize = Buffer.size();

        while (true) {
            size_t availSize = Buffer.capacity() - currSize;
            Buffer.resize(currSize + availSize);
    
            va_list args;
            va_start(args, fmt);
            size_t appSize = vsnprintf(Buffer.data() + currSize, availSize, fmt, args);
            va_end(args);
    
            if (appSize < availSize) {
                Buffer.resize(currSize + appSize);
                break;
            }
            Buffer.reserve(Buffer.capacity() * 2);
        }
    }
    
    void PrintTypeRef(slang::TypeReflection* type) {
        const char* name = type->getName();
        auto kind = type->getKind();

        switch (type->getKind()) {
            case slang::TypeReflection::Kind::Pointer: {
                auto elemType = type->getGenericContainer()->getConcreteType(type->getGenericContainer()->getTypeParameter(0));
                Append("havk::DevicePtr<");
                PrintTypeRef(elemType);
                Append(">");
                break;
            }
            case slang::TypeReflection::Kind::Vector:
            case slang::TypeReflection::Kind::Matrix: {
                Append("havk::vectors::");
                switch (type->getElementType()->getScalarType()) {
                    case slang::TypeReflection::Int32: Append("int"); break;
                    case slang::TypeReflection::UInt32: Append("uint"); break;
                    case slang::TypeReflection::Float16: Append("float16_t"); break;
                    case slang::TypeReflection::Float32: Append("float"); break;
                    default: PrintTypeRef(type->getElementType());
                }
                if (kind == slang::TypeReflection::Kind::Vector){
                    AppendFmt("%d", type->getColumnCount());
                } else {
                    AppendFmt("%dx%d", type->getRowCount(), type->getColumnCount());
                }
                break;
            }
            case slang::TypeReflection::Kind::Scalar: {
                switch (type->getScalarType()) {
                    case slang::TypeReflection::Int32: Append("int32_t"); break;
                    case slang::TypeReflection::UInt32: Append("uint32_t"); break;
                    case slang::TypeReflection::Float16: Append("havk::vectors::float16_t"); break;
                    default: Append(name); break;
                }
                break;
            }
            case slang::TypeReflection::Kind::Struct: {
                auto info = Types->GetInfo(type);
                // If we don't have info about this type, it's probably a builtin
                if (info == nullptr) {
                    if (IsBuiltinDescriptorHandle(type)) {
                        Append("havk::");
                        Append(type->getName());
                        break;
                    }
                    Slang::ComPtr<slang::IBlob> fullNameBlob;
                    type->getFullName(fullNameBlob.writeRef());

                    // "A.B.C" -> "A::B::"
                    const char* str = (const char*)fullNameBlob->getBufferPointer();
                    while (const char* end = strchr(str, '.')) {
                        Append(std::string_view(str, end));
                        Append("::");
                        str = end + 1;
                    }
                } else if (!info->Namespace.empty()) {
                    Append(info->Namespace);
                    Append("::");
                }
                Append(name);
                // TODO: should we care about generics?
                break;
            }
            default:
                Append(type->getName());
                fprintf(stderr, "warn: Don't know how to emit ref to type '%s' kind=%d\n", type->getName(), type->getKind());
                break;
        }
    }
    
    void PrintField(slang::TypeReflection* type, std::string_view name) {
        auto kind = type->getKind();

        if (type->isArray()) {
            PrintTypeRef(type->unwrapArray());
            AppendFmt(" %s", name.data());

            do {
                size_t size = type->getElementCount();
                AppendFmt(size == 0 ? "[]" : "[%zu]", size);   
                type = type->getElementType();
            } while (type->isArray());

            Append(";\n");
        } else {
            PrintTypeRef(type);
            AppendFmt(" %s;\n", name.data());
        }
    }
    void PrintTypeDef(slang::TypeReflection* type) {
        if (auto info = Types->GetInfo(type)) {
            SetNamespace(info->Namespace);
        }

        if (type->getKind() == slang::TypeReflection::Kind::Struct) {
            Begin("struct %s {\n", type->getName());

            for (uint32_t i = 0; i < type->getFieldCount(); i++) {
                auto field = type->getFieldByIndex(i);

                Indent();
                PrintField(field->getType(), field->getName());
            }
            End("};\n");
        } else {
            fprintf(stderr, "warn: Don't know how to emit decl for type '%s' kind=%d\n", type->getName(), type->getKind());
        }
    }
    void PrintImplicitParamStruct(slang::TypeLayoutReflection* type) {
        std::vector<slang::VariableLayoutReflection*> fields;
        
        for (uint32_t i = 0; i < type->getFieldCount(); i++) {
            slang::VariableLayoutReflection* field = type->getFieldByIndex(i);
            if (field->getCategory() == slang::ParameterCategory::Uniform) {
                fields.push_back(field);
            }
        }
        if (fields.size() == 1 && fields[0]->getType()->getKind() == slang::TypeReflection::Kind::Struct) {
            AppendFmt("\tusing Params = ");
            PrintTypeRef(fields[0]->getType());
            Append(";\n");
            return;
        }
        Begin("struct Params {\n");

        for (auto field : fields) {
            AppendFmt("\t/* %2d */ ", field->getOffset());
            PrintField(field->getType(), field->getName());
        }
        End("};\n");
    }
};

static void PrintDiags(slang::IBlob* blob) {
    if (blob != nullptr) {
        fprintf(stderr, "%s\n", (const char*)blob->getBufferPointer());
    }
}

static bool CompileShader(
    slang::IGlobalSession* globalSession, slang::ISession* session, 
    const std::filesystem::path& sourceFile, const std::filesystem::path& outputFile, const std::filesystem::path& baseDir,
    TypeGraph* typeGraph
) {
    // Parsing
    Slang::ComPtr<slang::IBlob> diagnostics;
    slang::IModule* module = session->loadModule((char*)sourceFile.u8string().data(), diagnostics.writeRef());

    PrintDiags(diagnostics);
    if (!module) return false;

    // Compositing
    std::vector<slang::IComponentType*> components = { module };
    std::vector<Slang::ComPtr<slang::IEntryPoint>> entryPoints;

    for (int32_t i = 0; i < module->getDefinedEntryPointCount(); i++) {
        module->getDefinedEntryPoint(i, entryPoints.emplace_back().writeRef());
        components.push_back(entryPoints.back());
    }
    Slang::ComPtr<slang::IComponentType> program;
    session->createCompositeComponentType(components.data(), (SlangInt)components.size(), program.writeRef(), diagnostics.writeRef());

    PrintDiags(diagnostics);
    if (!program) return false;

    // Linking
    Slang::ComPtr<slang::IComponentType> linkedProgram;
    program->link(linkedProgram.writeRef(), diagnostics.writeRef());
    PrintDiags(diagnostics);

    // Reflect
    slang::ProgramLayout* layout = linkedProgram->getLayout();
    slang::TypeLayoutReflection* globalPushConstType = nullptr;
    std::vector<slang::VariableLayoutReflection*> definedSpecConstants;

    for (uint32_t j = 0; j < layout->getParameterCount(); j++) {
        slang::VariableLayoutReflection* par = layout->getParameterByIndex(j);
        if (std::string_view(par->getName()).starts_with("havk__")) continue;

        if (par->getCategory() == slang::ParameterCategory::PushConstantBuffer) {
            if (globalPushConstType != nullptr) {
                fprintf(stderr, "error: Only either one push constant binding or uniform entry point parameters can be defined.\n");
                return false;
            }
            globalPushConstType = par->getTypeLayout()->getElementTypeLayout();
        } else if (par->getCategory() == slang::ParameterCategory::SpecializationConstant) {
            definedSpecConstants.push_back(par);
        }
    }

    // Code gen
    Slang::ComPtr<slang::IBlob> kernelBlob = nullptr;
    if (module->getDefinedEntryPointCount() > 0) {
        linkedProgram->getTargetCode(0, kernelBlob.writeRef(), diagnostics.writeRef());

        PrintDiags(diagnostics);
        if (!kernelBlob) return false;
    }

    std::string relativeSourcePath = std::filesystem::relative(sourceFile, baseDir).string();
    std::replace(relativeSourcePath.begin(), relativeSourcePath.end(), '\\', '/');
    std::filesystem::path outputFileMut = outputFile;

    // Populate type graph so we can query which modules/namespace decls are in.
    // Slang unfortunately does not provide a way to query that info from from types.
    for (uint32_t i = 0; i < module->getDependencyFileCount(); i++) {
        std::string_view depPath = module->getDependencyFilePath(i);
        if (depPath.ends_with("/Havk/Core.slang") || depPath.ends_with("\\Havk\\Core.slang")) continue;

        slang::IModule* depModule = session->loadModule(depPath.data());
        if (depModule == nullptr) continue;  // file is related to an #include

        typeGraph->RegisterTypes(depModule, depModule->getModuleReflection());
    }

    // Find all types we define and depend on, in topological / post DFS order
    std::vector<slang::TypeReflection*> sortedTypes;

    for (auto& [type, info] : typeGraph->Entries) {
        if (info.Module == module) {
            typeGraph->GetOrderedDependencies(type, sortedTypes);
        }
    }
    
    for (uint32_t i = 0; i < layout->getEntryPointCount(); i++) {
        slang::EntryPointReflection* entryReflect = layout->getEntryPointByIndex(i);
        slang::VariableLayoutReflection* sigLayout = entryReflect->getVarLayout();

        if (sigLayout->getCategory() == slang::ParameterCategory::PushConstantBuffer) {
            auto pcLayout = sigLayout->getTypeLayout()->getElementTypeLayout();

            for (uint32_t i = 0; i < pcLayout->getFieldCount(); i++) {
                typeGraph->GetOrderedDependencies(pcLayout->getFieldByIndex(i)->getType(), sortedTypes);
            }
        }
    }
    if (globalPushConstType != nullptr) {
        typeGraph->GetOrderedDependencies(globalPushConstType->getType(), sortedTypes);
    }
    
    // Generate bridge code
    // SPIR-V data is defined in a separate compilation unit to avoid triggering recompilation of dependent sources.
    CodePrinter headerCode(typeGraph);
    CodePrinter unitCode(typeGraph);

    headerCode.Append("// This file has been auto generated. Any changes will be lost on next rebuild.\n");

    for (int32_t i = 0; i < module->getDependencyFileCount(); i++) {
        headerCode.AppendFmt("// DEP: %s\n", std::filesystem::canonical(module->getDependencyFilePath(i)).u8string().data());
    }

    headerCode.Append("\n#pragma once\n");
    headerCode.Append("#include <Havk/ShaderBridge.h>\n");

    unitCode.AppendFmt("#include \"%s\"\n", outputFileMut.replace_extension(".h").u8string().data());
    
    std::unordered_set<slang::IModule*> includedModules;

    for (auto& type : sortedTypes) {
        auto info = typeGraph->GetInfo(type);

        if (info != nullptr && info->Module != module && includedModules.insert(info->Module).second) {
            auto path = std::filesystem::relative(info->Module->getFilePath(), sourceFile.parent_path());
            std::string relPath = path.replace_extension().string();
            std::replace(relPath.begin(), relPath.end(), '\\', '/');
            headerCode.AppendFmt("#include \"%s.h\"\n", relPath.data());
        }
    }

    // Generate types defined in this module
    for (auto& type : sortedTypes) {
        auto info = typeGraph->GetInfo(type);

        if (info != nullptr && info->Module == module) {
            headerCode.PrintTypeDef(type);
        }
    }

    if (module->getDefinedEntryPointCount() > 0) {
        UpdateFile(outputFileMut.replace_extension(".spv"), kernelBlob->getBufferPointer(), kernelBlob->getBufferSize());
        
        Slang::ComPtr<slang::IBlob> reflectJson = nullptr;
        layout->toJson(reflectJson.writeRef());
        UpdateFile(std::filesystem::path(outputFileMut).replace_extension(".reflect.json"), reflectJson->getBufferPointer(), reflectJson->getBufferSize());

#if 0
        unitCode.Append("\n#if __clang__\n#pragma clang diagnostic ignored \"-Wc23-extensions\"\n#endif\n\n");
        unitCode.Append("static const uint8_t g_ModuleSpirvCode[] = {\n");
        unitCode.AppendFmt("    #embed \"%s\"", ChangeExtension(outputFile, ".spv").filename().string().data());
#else
        unitCode.Append("static const uint32_t g_ModuleSpirvCode[] = {");
        auto spirvData = (const uint32_t*)kernelBlob->getBufferPointer();
        uint32_t spirvWordCount = kernelBlob->getBufferSize() / 4;
        for (uint32_t i = 0; i < spirvWordCount; i++) {
            if (i % 8 == 0) unitCode.Append("\n    ");
            unitCode.AppendFmt("0x%08X, ", spirvData[i]);
        }
#endif
        unitCode.Append("\n};\n");
        
        // Header

        for (uint32_t i = 0; i < layout->getEntryPointCount(); i++) {
            slang::EntryPointReflection* entryReflect = layout->getEntryPointByIndex(i);
            auto ns = typeGraph->ParentNamespaces.at(entryReflect->getFunction());

            headerCode.SetNamespace(ns);
            unitCode.SetNamespace(ns);

            headerCode.Begin("struct %s {\n", entryReflect->getName());

            slang::TypeLayoutReflection* pcLayout = globalPushConstType;
            slang::VariableLayoutReflection* sigLayout = entryReflect->getVarLayout();
            bool isImplicitPcStruct = false;
            
            if (sigLayout->getCategory() == slang::ParameterCategory::PushConstantBuffer) {
                if (globalPushConstType != nullptr) {
                    fprintf(stderr, "error: Only either one push constant binding or uniform entry point parameters can be defined.");
                    return false;
                }
                pcLayout = sigLayout->getTypeLayout()->getElementTypeLayout();
                isImplicitPcStruct = true;
            }
            if (isImplicitPcStruct) {
                headerCode.PrintImplicitParamStruct(pcLayout);
            } else if (pcLayout != nullptr) {
                headerCode.AppendFmt("\tusing Params = %s;\n", pcLayout->getName());
            } else {
                headerCode.AppendFmt("\tusing Params = void;\n");
            }

            headerCode.AppendFmt("\tstatic const havk::ModuleDesc Module;\n");

            unitCode.Begin("const havk::ModuleDesc %s::Module = {\n", entryReflect->getName());
            unitCode.AppendFmt("\t.Code = g_ModuleSpirvCode,\n");
            unitCode.AppendFmt("\t.CodeSize = sizeof(g_ModuleSpirvCode),\n");
            unitCode.AppendFmt("\t.EntryPoint = \"%s\",\n", entryReflect->getName());
            unitCode.AppendFmt("\t.SourcePath = \"%s\",\n", relativeSourcePath.data());
            unitCode.End("};\n");

            if (entryReflect->getStage() == SLANG_STAGE_COMPUTE || entryReflect->getStage() == SLANG_STAGE_MESH) {
                SlangUInt numThreads[4] = {};
                entryReflect->getComputeThreadGroupSize(3, numThreads);
                entryReflect->getComputeWaveSize(&numThreads[3]);

                headerCode.AppendFmt("\tstatic constexpr havk::vectors::uint3 GroupSize = { %d, %d, %d };\n", numThreads[0], numThreads[1], numThreads[2]);
                if (numThreads[3] != 0) {
                    headerCode.AppendFmt("\tstatic constexpr uint32_t WaveSize = %d;\n", numThreads[3]);
                }
            }

            // Generate spec constants
            std::vector<slang::VariableLayoutReflection*> usedSpecConstants;

            if (definedSpecConstants.size() > 0) {
                Slang::ComPtr<slang::IMetadata> metadata;
                linkedProgram->getEntryPointMetadata(i, 0, metadata.writeRef());

                for (auto* par : definedSpecConstants) {
                    bool isUsed;
                    metadata->isParameterLocationUsed((SlangParameterCategory)par->getCategory(),
                                                      par->getBindingSpace(), par->getBindingIndex(), isUsed);
                    if (isUsed) usedSpecConstants.push_back(par);
                }
            }

            if (usedSpecConstants.size() == 0) {
                headerCode.AppendFmt("\tusing SpecConst = void;\n");
            } else {
                headerCode.Begin("struct SpecConst {\n");
                for (auto* par : usedSpecConstants) {
                    headerCode.AppendFmt("\thavk::SpecOpt<");
                    headerCode.PrintTypeRef(par->getType());
                    headerCode.AppendFmt("> %s;\n", par->getName());
                }
                headerCode.Append("\n");

                headerCode.Begin("operator havk::SpecConstMap() const {\n");
                headerCode.AppendFmt("\thavk::SpecConstMap map;\n");
                for (auto* par : usedSpecConstants) {
                    const char* name = par->getName();
                    headerCode.AppendFmt("\tif (%s.present) map.Add(%d, %s.value);\n", name, par->getBindingIndex(), name);
                }
                headerCode.AppendFmt("\treturn map;\n");
                headerCode.End("}\n");

                headerCode.End("};\n");
            }

            headerCode.End("};\n");
        }
    }
    headerCode.SetNamespace("");
    unitCode.SetNamespace("");

    UpdateFile(outputFileMut.replace_extension(".h"), headerCode.Buffer.data(), headerCode.Buffer.size());
    UpdateFile(outputFileMut.replace_extension(".cpp"), unitCode.Buffer.data(), unitCode.Buffer.size());
    
    return true;
}

static std::vector<std::string> ParseDependencyList(const std::filesystem::path& sourcePath) {
    std::vector<std::string> result;
    std::ifstream is(sourcePath);

    for (std::string line; std::getline(is, line); ) {
        if (line.starts_with("// DEP: ")) {
            result.push_back(line.substr(strlen("// DEP: ")));
        } else if (!line.starts_with("// ")) {
            break;
        }
    }
    return result;
}

int main(int argc, const char** args) {
    #if _WIN32
    setvbuf(stdout, NULL, _IONBF, 0);
    #else
    setlinebuf(stdout);
    #endif

    std::filesystem::path baseDir = std::filesystem::current_path();
    std::filesystem::path outputDir = baseDir;
    std::string baseNamespace = "";
    int optLevel = SLANG_OPTIMIZATION_LEVEL_NONE;
    int debugLevel = SLANG_DEBUG_INFO_LEVEL_NONE;
    SlangMatrixLayoutMode matrixLayout = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    bool skipUnchanged = false;
    bool watchChanges = false;
    bool verbose = false;

    std::vector<const char*> includeDirs;
    std::vector<slang::PreprocessorMacroDesc> prepDefs;
    std::vector<std::unique_ptr<char[]>> prefNameDefs;
    std::vector<slang::CompilerOptionEntry> options;

    int argi = 1;
    while (argi < argc) {
        std::string_view arg = args[argi++];
        if (!arg.starts_with("-")) { argi--; break; }

        if (arg == "--output-dir") {
            outputDir = args[argi++];
        }
        else if (arg == "--base-dir") {
            baseDir = args[argi++];
        }
        else if (arg == "--base-ns") {
            baseNamespace = args[argi++];
        }
        else if (arg.starts_with("-I")) {
            includeDirs.push_back(arg.substr(2).data());
        }
        else if (arg.starts_with("-D")) {
            size_t valPos = arg.find('=');

            if (valPos == std::string::npos) {
                prepDefs.push_back({ .name = &arg[2], .value = "" });
            } else {
                char* name = prefNameDefs.emplace_back(new char[valPos - 2 + 1]).get();  // must malloc for stable address
                strncpy(name, &arg[2], valPos - 2);
                prepDefs.push_back({ .name = name, .value = &arg[valPos + 1] });
            }
        }
        else if (arg.starts_with("-O")) {
            optLevel = std::clamp(arg[2] - '0', 0, 3);
        }
        else if (arg.starts_with("-g")) {
            debugLevel = std::clamp(arg[2] - '0', 0, 3);
        }
        else if (arg == "--row-major") {
            matrixLayout = SLANG_MATRIX_LAYOUT_ROW_MAJOR;
        }
        else if (arg == "--skip-unchanged") {
            skipUnchanged = true;
        }
        else if (arg == "--watch") {
            watchChanges = true;
        }
        else if (arg == "--verbose") {
            verbose = true;
        }
        else {
            fprintf(stderr, "Unknown argument: '%s'\n", arg.data());
            return 1;
        }
    }
    
    if ((argi == argc && !watchChanges) || outputDir.empty()) {
        printf("Usage:\n");
        printf("  ShaderBuildTool <options> <input files>\n");
        printf("  --output-dir <path>       Output directory for compiled shaders and headers (required)\n");
        printf("  --base-dir <path>         Base directory for relative source files (defaults to cwd)\n");
        printf("  --base-ns <string>        Base namespace for generated header files\n");
        printf("  -I<path>                  Add preprocessor include search directory\n");
        printf("  -D<key>[=value]           Add preprocessor definition\n");
        printf("  -O<level=0..3>            Optimization level for spirv-opt.\n");
        printf("  -g<level=0..3>            Embed debug info in compiled binaries.\n");
        printf("  --row-major               Set default matrix ordering to row-major.\n");
        printf("  --skip-unchanged          Skip compilation of unchanged sources (comparing by binary timestamp)\n");
        printf("  --watch                   Watch for changes in source directories and print paths to stdout.\n");
        printf("  --verbose                 Print extra debug information.\n");
        return 1;
    }

    Slang::ComPtr<slang::IGlobalSession> globalSession;
    slang::createGlobalSession(globalSession.writeRef());

    options.push_back({ slang::CompilerOptionName::ForceCLayout, { .intValue0 = 1 } });
    options.push_back({ slang::CompilerOptionName::VulkanUseEntryPointName, { .intValue0 = 1 } });
    options.push_back({ slang::CompilerOptionName::Optimization, { .intValue0 = optLevel } });
    options.push_back({ slang::CompilerOptionName::DebugInformation, { .intValue0 = debugLevel } });
    options.push_back({ slang::CompilerOptionName::Capability, { .intValue0 = globalSession->findCapability("vk_mem_model") } });

    slang::TargetDesc targetDesc = {
        .format = SLANG_SPIRV,
        .profile = globalSession->findProfile("spirv_1_6"),
        .flags = SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY | SLANG_TARGET_FLAG_GENERATE_WHOLE_PROGRAM,
        .compilerOptionEntries = options.data(),
        .compilerOptionEntryCount = (uint32_t)options.size(),
    };
    slang::SessionDesc sessionDesc = {
        .targets = &targetDesc,
        .targetCount = 1,
        .defaultMatrixLayoutMode = matrixLayout,
        .searchPaths = includeDirs.data(),
        .searchPathCount = (uint32_t)includeDirs.size(),
        .preprocessorMacros = prepDefs.data(),
        .preprocessorMacroCount = (uint32_t)prepDefs.size(),
        .compilerOptionEntries = options.data(),
        .compilerOptionEntryCount = (uint32_t)options.size(),
    };

    bool hasError = false;

    auto CompileSource = [&](std::filesystem::path sourceFile, slang::ISession* session, TypeGraph& typeGraph) {
        if (!sourceFile.is_absolute()) {
            sourceFile = baseDir / sourceFile;
        } else if (!IsSubpath(sourceFile, baseDir)) {
            fprintf(stderr, "Source file '%s' is not inside base directory.\n", sourceFile.string().data());
            hasError = true;
            return false;
        }
        if (!std::filesystem::exists(sourceFile)) {
            fprintf(stderr, "Source file '%s' does not exist.\n", sourceFile.string().data());
            hasError = true;
            return false;
        }
        std::filesystem::path outputFile = outputDir / std::filesystem::relative(sourceFile, baseDir);
        outputFile.replace_extension(".spv");

        if (skipUnchanged && !watchChanges) {
            std::error_code ec;
            auto sourceTs = std::filesystem::last_write_time(sourceFile, ec);
            auto binaryTs = sourceTs.min();

            std::filesystem::path headerFile = outputFile;
            headerFile.replace_extension(".h");
            
            if (std::filesystem::exists(headerFile)) {
                binaryTs = std::filesystem::last_write_time(headerFile, ec);

                for (auto& depPath : ParseDependencyList(headerFile)) {
                    auto depTs = std::filesystem::last_write_time(depPath, ec);
                    if (!ec && depTs > sourceTs) sourceTs = depTs;
                }
            }

            if (sourceTs <= binaryTs) {
                if (verbose) printf("Up to date: '%s'\n", sourceFile.string().data());
                return true;
            }
        }

        printf("Building %s\n", std::filesystem::relative(sourceFile, baseDir).string().data());
        if (!CompileShader(globalSession.get(), session, sourceFile, outputFile, baseDir, &typeGraph)) {
            fprintf(stderr, "error: failed to compile shader '%s'\n", std::filesystem::path(sourceFile).filename().string().data());
            return false;
        }
        return true;
    };

    if (!watchChanges) {
        Slang::ComPtr<slang::ISession> session;
        globalSession->createSession(sessionDesc, session.writeRef());
        TypeGraph typeGraph = { .BaseNamespace = baseNamespace };

        while (argi < argc) {
            if (!CompileSource(args[argi++], session.get(), typeGraph)) {
                hasError = true;
            }
        }
    } else {
        printf("Watching for changes in '%s'...\n", baseDir.string().data());

        // Create mapping of source -> dependent files
        std::unordered_multimap<std::filesystem::path, std::filesystem::path> depMap;

        for (auto& entry : std::filesystem::recursive_directory_iterator(outputDir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".h") continue;
    
            auto deps = ParseDependencyList(entry.path());
            if (deps.empty()) continue;

            depMap.insert({ deps[0], deps[0] }); // convenience: source maps to itself
    
            for (uint32_t i = 1; i < deps.size(); i++) {
                depMap.insert({ deps[i], deps[0] });
            }
        }

        auto watcher = havx::FileWatcher(std::string_view((char*)baseDir.u8string().data()));

        while (true) {
            watcher.WaitForChanges(-1);
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // delay a bit to avoid duplicate events

            std::vector<std::string> changedFiles;
            watcher.PollChanges(changedFiles);

            Slang::ComPtr<slang::ISession> session;
            globalSession->createSession(sessionDesc, session.writeRef());
            TypeGraph typeGraph = { .BaseNamespace = baseNamespace };

            for (std::string& path : changedFiles) {
                auto iter = depMap.equal_range(std::filesystem::absolute(baseDir / std::filesystem::path((char8_t*)path.data())));

                for (; iter.first != iter.second; iter.first++) {
                    auto& sourceFile = iter.first->second;
                    if (CompileSource(sourceFile, session.get(), typeGraph)) {
                        std::filesystem::path outputFile = outputDir / std::filesystem::relative(sourceFile, baseDir);
                        outputFile.replace_extension(".spv");
                        printf("Recompiled %s -> %s\n", sourceFile.string().data(), (char*)outputFile.u8string().data());
                    }
                }
            }
        }
    }
    return hasError ? 1 : 0;
}