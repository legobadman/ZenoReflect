#include <fstream>
#include "args.hpp"
#include "log.hpp"
#include "utils.hpp"
#include "parser.hpp"
#include "serialize.hpp"
#include "codegen.hpp"
#include "template/template_literal"

using namespace llvm;
using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;

class ReflectionGeneratorAction : public ASTFrontendAction {
public:
    ReflectionGeneratorAction(zeno::reflect::CodeCompilerState& compielr_state, std::string header_path): m_compiler_state(compielr_state), m_header_path(std::move(header_path)) {}

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &compiler, StringRef code) override {
        return std::make_unique<ReflectionASTConsumer>(m_compiler_state, m_header_path);
    }

private:
    zeno::reflect::CodeCompilerState& m_compiler_state;
    std::string m_header_path;
};

ParserErrorCode generate_reflection_model(const TranslationUnit &unit, ReflectionModel &out_model, zeno::reflect::CodeCompilerState& root_state) {
    out_model.debug_name = unit.identity_name;
    std::vector<std::string> args = zeno::reflect::get_parser_command_args(GLOBAL_CONTROL_FLAGS->cpp_version, GLOBAL_CONTROL_FLAGS->include_dirs, GLOBAL_CONTROL_FLAGS->pre_include_headers, GLOBAL_CONTROL_FLAGS->verbose);

    const std::string gen_template_header_path = zeno::reflect::get_file_path_in_header_output(std::format("reflect/{}.generated.hpp", zeno::reflect::normalize_filename(unit.identity_name)));
    zeno::reflect::truncate_file(gen_template_header_path);
    out_model.generated_headers.insert(gen_template_header_path);

    if (!clang::tooling::runToolOnCodeWithArgs(
        std::make_unique<ReflectionGeneratorAction>(root_state, gen_template_header_path),
        unit.source.c_str(),
        args,
        unit.identity_name.c_str()
    )) {
        return ParserErrorCode::InternalError;
    }

    return ParserErrorCode::Success;
}

ParserErrorCode post_generate_reflection_model(const ReflectionModel &model, const zeno::reflect::CodeCompilerState& state)
{
    const std::string generated_header_path = zeno::reflect::get_file_path_in_header_output("reflect/reflection.generated.hpp");
    std::ofstream ghp_stream(generated_header_path, std::ios::out | std::ios::trunc);
    ghp_stream << "#pragma once" << "\r\n";
    for (const std::string& s : model.generated_headers) {
        ghp_stream << std::format("#include \"{}\"", zeno::reflect::relative_path_to_header_output(s)) << "\r\n";
    }

    const std::string generated_target_source = GLOBAL_CONTROL_FLAGS->target_type_register_source_path;
    std::ofstream gts_stream(generated_target_source, std::ios::out | std::ios::trunc);
    gts_stream << inja::render(zeno::reflect::text::REFLECTED_TYPE_REGISTER, state.types_register_data);

    return ParserErrorCode::Success;
}

ParserErrorCode pre_generate_reflection_model()
{
    const std::string generated_header_path = zeno::reflect::get_file_path_in_header_output("reflect/reflection.generated.hpp");
    zeno::reflect::truncate_file(generated_header_path);

    return ParserErrorCode::Success;
}

TypeAliasMatchCallback::TypeAliasMatchCallback(ReflectionASTConsumer *context) : m_context(context) {}

void TypeAliasMatchCallback::run(const MatchFinder::MatchResult &result)
{
    const TypeDecl* decll = nullptr;
    QualType underlying_type;
    if (const TypedefDecl* decl = result.Nodes.getNodeAs<TypedefDecl>(ASTLabels::TYPEDEF_LABEL)) {
        underlying_type = decl->getUnderlyingType();
        decll = decl;
    } else if (const TypeAliasDecl* decl = result.Nodes.getNodeAs<TypeAliasDecl>(ASTLabels::TYPE_ALIAS_LABEL)) {
        underlying_type = decl->getUnderlyingType();
        decll = decl;
    }

    if (decll && m_context) {
        // zeno::reflect::RTTITypeGenerator rtti(underlying_type);
        // m_context->template_header_generator.add_rtti_block(rtti);
    }
}

RecordTypeMatchCallback::RecordTypeMatchCallback(ReflectionASTConsumer *context) : m_context(context) {}

void RecordTypeMatchCallback::run(const MatchFinder::MatchResult &result)
{
    if (const CXXRecordDecl* record_decl = result.Nodes.getNodeAs<CXXRecordDecl>(ASTLabels::RECORD_LABEL)) {
        if (!record_decl->hasDefinition()) {
            if (GLOBAL_CONTROL_FLAGS->verbose) {
                llvm::outs() << "[debug] «" << record_decl->getNameAsString() << "» is only a declaration, skipped.\n";
            }
            return;
        }

        bool is_reflected_record_type = false;
        MetadataContainer container;

        if (record_decl->hasAttrs()) {
            for (const auto* attr : record_decl->attrs()) {
                if (const AnnotateAttr *Annotate = dyn_cast<AnnotateAttr>(attr)) {
                    container = MetadataParser::parse(Annotate->getAnnotation().str());
                    is_reflected_record_type = true;
                    break;
                }
            }
        }

        if (!is_reflected_record_type) {
            return;
        }
        
        // Generate rtti information
        const Type* record_type = record_decl->getTypeForDecl();
        QualType record_qual_type(record_type, 0);
        m_context->template_header_generator.add_rtti_type(record_qual_type);

        {
            const std::string normalized_name = zeno::reflect::convert_to_valid_cpp_var_name(record_qual_type.getCanonicalType().getAsString());
            bool found = false;
            for (auto type_info : m_context->m_compiler_state.types_register_data["types"]) {
                if (type_info["normal_name"] == normalized_name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                inja::json type_data;
                type_data["normal_name"] = normalized_name;
                type_data["qualified_name"] = record_qual_type.getAsString();
                type_data["canonical_typename"] = record_qual_type.getCanonicalType().getAsString();
                type_data["ctors"] = {};
                type_data["funcs"] = {};

                // Processing methods
                {
                    unsigned int num_ctor = 0;
                    for (auto it = record_decl->method_begin(); it != record_decl->method_end(); ++it) {
                        // Register all param types used
                        if (const CXXMethodDecl* method_decl = dyn_cast<CXXMethodDecl>(*it)) {
                            for (unsigned int i = 0; i < method_decl->getNumParams(); ++i) {
                                const ParmVarDecl* param_decl = method_decl->getParamDecl(i);
                                if (param_decl) {
                                    QualType type = param_decl->getType();
                                    m_context->template_header_generator.add_rtti_type(type);
                                    m_context->template_header_generator.add_rtti_type(type.getUnqualifiedType());
                                }
                            }
                            QualType type = method_decl->getReturnType().getCanonicalType();
                            m_context->template_header_generator.add_rtti_type(type);
                            m_context->template_header_generator.add_rtti_type(type.getUnqualifiedType());
                        }

                        if (const CXXConstructorDecl* constructor_decl = dyn_cast<CXXConstructorDecl>(*it)) {
                            ++num_ctor;

                            std::vector<std::string> ctor_params;
                            for (unsigned int i = 0; i < constructor_decl->getNumParams(); ++i) {
                                const ParmVarDecl* param_decl = constructor_decl->getParamDecl(i);
                                if (param_decl) {
                                    QualType type = param_decl->getType();
                                    ctor_params.push_back(type.getCanonicalType().getAsString());
                                }
                            }
                            type_data["ctors"].push_back(ctor_params);
                        } else if (const CXXDestructorDecl* destructor_decl = dyn_cast<CXXDestructorDecl>(*it)) {
                        } else if (const CXXConversionDecl* conversion_decl = dyn_cast<CXXConversionDecl>(*it)) {
                        } else if (const CXXMethodDecl* method_decl = dyn_cast<CXXMethodDecl>(*it)) {
                            inja::json func_data;
                            func_data["name"] = method_decl->getNameAsString();
                            func_data["ret"] = method_decl->getReturnType().getCanonicalType().getAsString();
                            func_data["params"] = {};
                            for (unsigned int i = 0; i < method_decl->getNumParams(); ++i) {
                                const ParmVarDecl* param_decl = method_decl->getParamDecl(i);
                                if (param_decl) {
                                    QualType type = param_decl->getType();
                                    func_data["params"].push_back(type.getCanonicalType().getAsString());
                                }
                            }
                            func_data["static"] = method_decl->isStatic();
                            func_data["const"] = method_decl->isConst();
                            func_data["noexcept"] = method_decl->getExceptionSpecType() == clang::EST_BasicNoexcept || method_decl->getExceptionSpecType() == clang::EST_NoexceptTrue;
                            type_data["funcs"].push_back(func_data);
                        }
                    }
                    type_data["num_ctor"] = num_ctor;
                }

                {
                    for (auto it = record_decl->field_begin(); it != record_decl->field_end(); ++it) {
                        if (const FieldDecl* field_decl = dyn_cast<FieldDecl>(*it)) {
                            QualType type = field_decl->getType();
                            m_context->template_header_generator.add_rtti_type(type);
                            m_context->template_header_generator.add_rtti_type(type.getUnqualifiedType());

                            inja::json field_data;
                            field_data["name"] = field_decl->getNameAsString();
                            field_data["type"] = type.getCanonicalType().getAsString();
                            field_data["normal_type"] = zeno::reflect::convert_to_valid_cpp_var_name(type.getCanonicalType().getAsString());
                        }
                    }
                }

                m_context->m_compiler_state.types_register_data["types"].push_back(type_data);
            }
        }

        if (record_decl->getNumBases() > 0) {
            for (const auto& base : record_decl->bases()) {
            }
        }
    }
}

ReflectionASTConsumer::ReflectionASTConsumer(zeno::reflect::CodeCompilerState &state, std::string header_path)
    : m_compiler_state(state)
    , m_header_path(header_path)
    , template_header_generator(zeno::reflect::TemplateHeaderGenerator{state})
{
}

void ReflectionASTConsumer::HandleTranslationUnit(ASTContext &context)
{
    scoped_context = &context;
    // template_header_generator.add_rtti_type(context.VoidTy);

    const std::string& gen_template_header_path = m_header_path;

    // MatchFinder type_alias_finder{};
    // DeclarationMatcher typealias_matcher = typeAliasDecl().bind(ASTLabels::TYPE_ALIAS_LABEL);
    // DeclarationMatcher typedef_matcher = typedefDecl().bind(ASTLabels::TYPEDEF_LABEL);
    // type_alias_finder.addMatcher(typealias_matcher, type_alias_handler.get());
    // type_alias_finder.addMatcher(typedef_matcher, type_alias_handler.get());
    // type_alias_finder.matchAST(context);

    DeclarationMatcher record_type_matcher = cxxRecordDecl().bind(ASTLabels::RECORD_LABEL);
    MatchFinder record_finder{};
    record_finder.addMatcher(record_type_matcher, record_type_handler.get());
    record_finder.matchAST(context);

    // generate header
    const std::string generated_templates = template_header_generator.compile();
    
    std::ofstream generated_templates_stream(gen_template_header_path, std::ios::out | std::ios::trunc);
    generated_templates_stream << generated_templates;

    scoped_context = nullptr;
}

void ReflectionASTConsumer::add_type_mapping(const std::string &alias_name, QualType real_name)
{
    if (GLOBAL_CONTROL_FLAGS->verbose) {
        llvm::outs() << "[debug] " << "Added type alias: «" << alias_name << "» → «" << real_name << "»\n";
    }
    type_name_mapping.insert_or_assign(alias_name, real_name);
}
