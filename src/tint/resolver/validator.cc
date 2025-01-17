// Copyright 2020 The Tint Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/tint/resolver/validator.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "src/tint/ast/alias.h"
#include "src/tint/ast/array.h"
#include "src/tint/ast/assignment_statement.h"
#include "src/tint/ast/bitcast_expression.h"
#include "src/tint/ast/break_statement.h"
#include "src/tint/ast/call_statement.h"
#include "src/tint/ast/continue_statement.h"
#include "src/tint/ast/depth_texture.h"
#include "src/tint/ast/disable_validation_attribute.h"
#include "src/tint/ast/discard_statement.h"
#include "src/tint/ast/fallthrough_statement.h"
#include "src/tint/ast/for_loop_statement.h"
#include "src/tint/ast/id_attribute.h"
#include "src/tint/ast/if_statement.h"
#include "src/tint/ast/internal_attribute.h"
#include "src/tint/ast/interpolate_attribute.h"
#include "src/tint/ast/loop_statement.h"
#include "src/tint/ast/matrix.h"
#include "src/tint/ast/pointer.h"
#include "src/tint/ast/return_statement.h"
#include "src/tint/ast/sampled_texture.h"
#include "src/tint/ast/sampler.h"
#include "src/tint/ast/storage_texture.h"
#include "src/tint/ast/switch_statement.h"
#include "src/tint/ast/traverse_expressions.h"
#include "src/tint/ast/type_name.h"
#include "src/tint/ast/unary_op_expression.h"
#include "src/tint/ast/variable_decl_statement.h"
#include "src/tint/ast/vector.h"
#include "src/tint/ast/workgroup_attribute.h"
#include "src/tint/sem/abstract_numeric.h"
#include "src/tint/sem/array.h"
#include "src/tint/sem/atomic.h"
#include "src/tint/sem/call.h"
#include "src/tint/sem/depth_multisampled_texture.h"
#include "src/tint/sem/depth_texture.h"
#include "src/tint/sem/for_loop_statement.h"
#include "src/tint/sem/function.h"
#include "src/tint/sem/if_statement.h"
#include "src/tint/sem/loop_statement.h"
#include "src/tint/sem/materialize.h"
#include "src/tint/sem/member_accessor_expression.h"
#include "src/tint/sem/multisampled_texture.h"
#include "src/tint/sem/pointer.h"
#include "src/tint/sem/reference.h"
#include "src/tint/sem/sampled_texture.h"
#include "src/tint/sem/sampler.h"
#include "src/tint/sem/statement.h"
#include "src/tint/sem/storage_texture.h"
#include "src/tint/sem/struct.h"
#include "src/tint/sem/switch_statement.h"
#include "src/tint/sem/type_constructor.h"
#include "src/tint/sem/type_conversion.h"
#include "src/tint/sem/variable.h"
#include "src/tint/sem/while_statement.h"
#include "src/tint/utils/defer.h"
#include "src/tint/utils/map.h"
#include "src/tint/utils/math.h"
#include "src/tint/utils/reverse.h"
#include "src/tint/utils/scoped_assignment.h"
#include "src/tint/utils/string.h"
#include "src/tint/utils/transform.h"

namespace tint::resolver {
namespace {

bool IsValidStorageTextureDimension(ast::TextureDimension dim) {
    switch (dim) {
        case ast::TextureDimension::k1d:
        case ast::TextureDimension::k2d:
        case ast::TextureDimension::k2dArray:
        case ast::TextureDimension::k3d:
            return true;
        default:
            return false;
    }
}

bool IsValidStorageTextureTexelFormat(ast::TexelFormat format) {
    switch (format) {
        case ast::TexelFormat::kR32Uint:
        case ast::TexelFormat::kR32Sint:
        case ast::TexelFormat::kR32Float:
        case ast::TexelFormat::kRg32Uint:
        case ast::TexelFormat::kRg32Sint:
        case ast::TexelFormat::kRg32Float:
        case ast::TexelFormat::kRgba8Unorm:
        case ast::TexelFormat::kRgba8Snorm:
        case ast::TexelFormat::kRgba8Uint:
        case ast::TexelFormat::kRgba8Sint:
        case ast::TexelFormat::kRgba16Uint:
        case ast::TexelFormat::kRgba16Sint:
        case ast::TexelFormat::kRgba16Float:
        case ast::TexelFormat::kRgba32Uint:
        case ast::TexelFormat::kRgba32Sint:
        case ast::TexelFormat::kRgba32Float:
            return true;
        default:
            return false;
    }
}

// Helper to stringify a pipeline IO attribute.
std::string attr_to_str(const ast::Attribute* attr) {
    std::stringstream str;
    if (auto* builtin = attr->As<ast::BuiltinAttribute>()) {
        str << "builtin(" << builtin->builtin << ")";
    } else if (auto* location = attr->As<ast::LocationAttribute>()) {
        str << "location(" << location->value << ")";
    }
    return str.str();
}

template <typename CALLBACK>
void TraverseCallChain(diag::List& diagnostics,
                       const sem::Function* from,
                       const sem::Function* to,
                       CALLBACK&& callback) {
    for (auto* f : from->TransitivelyCalledFunctions()) {
        if (f == to) {
            callback(f);
            return;
        }
        if (f->TransitivelyCalledFunctions().Contains(to)) {
            TraverseCallChain(diagnostics, f, to, callback);
            callback(f);
            return;
        }
    }
    TINT_ICE(Resolver, diagnostics) << "TraverseCallChain() 'from' does not transitively call 'to'";
}

}  // namespace

Validator::Validator(ProgramBuilder* builder, SemHelper& sem)
    : symbols_(builder->Symbols()), diagnostics_(builder->Diagnostics()), sem_(sem) {}

Validator::~Validator() = default;

void Validator::AddError(const std::string& msg, const Source& source) const {
    diagnostics_.add_error(diag::System::Resolver, msg, source);
}

void Validator::AddWarning(const std::string& msg, const Source& source) const {
    diagnostics_.add_warning(diag::System::Resolver, msg, source);
}

void Validator::AddNote(const std::string& msg, const Source& source) const {
    diagnostics_.add_note(diag::System::Resolver, msg, source);
}

// https://gpuweb.github.io/gpuweb/wgsl/#plain-types-section
bool Validator::IsPlain(const sem::Type* type) const {
    return type->is_scalar() ||
           type->IsAnyOf<sem::Atomic, sem::Vector, sem::Matrix, sem::Array, sem::Struct>();
}

// https://gpuweb.github.io/gpuweb/wgsl/#fixed-footprint-types
bool Validator::IsFixedFootprint(const sem::Type* type) const {
    return Switch(
        type,                                      //
        [&](const sem::Vector*) { return true; },  //
        [&](const sem::Matrix*) { return true; },  //
        [&](const sem::Atomic*) { return true; },
        [&](const sem::Array* arr) {
            return !arr->IsRuntimeSized() && IsFixedFootprint(arr->ElemType());
        },
        [&](const sem::Struct* str) {
            for (auto* member : str->Members()) {
                if (!IsFixedFootprint(member->Type())) {
                    return false;
                }
            }
            return true;
        },
        [&](Default) { return type->is_scalar(); });
}

// https://gpuweb.github.io/gpuweb/wgsl.html#host-shareable-types
bool Validator::IsHostShareable(const sem::Type* type) const {
    if (type->IsAnyOf<sem::I32, sem::U32, sem::F32, sem::F16>()) {
        return true;
    }
    return Switch(
        type,  //
        [&](const sem::Vector* vec) { return IsHostShareable(vec->type()); },
        [&](const sem::Matrix* mat) { return IsHostShareable(mat->type()); },
        [&](const sem::Array* arr) { return IsHostShareable(arr->ElemType()); },
        [&](const sem::Struct* str) {
            for (auto* member : str->Members()) {
                if (!IsHostShareable(member->Type())) {
                    return false;
                }
            }
            return true;
        },
        [&](const sem::Atomic* atomic) { return IsHostShareable(atomic->Type()); });
}

// https://gpuweb.github.io/gpuweb/wgsl.html#storable-types
bool Validator::IsStorable(const sem::Type* type) const {
    return IsPlain(type) || type->IsAnyOf<sem::Texture, sem::Sampler>();
}

const ast::Statement* Validator::ClosestContinuing(bool stop_at_loop,
                                                   sem::Statement* current_statement) const {
    for (const auto* s = current_statement; s != nullptr; s = s->Parent()) {
        if (stop_at_loop && s->Is<sem::LoopStatement>()) {
            break;
        }
        if (s->Is<sem::LoopContinuingBlockStatement>()) {
            return s->Declaration();
        }
        if (auto* f = As<sem::ForLoopStatement>(s->Parent())) {
            if (f->Declaration()->continuing == s->Declaration()) {
                return s->Declaration();
            }
            if (stop_at_loop) {
                break;
            }
        }
        if (Is<sem::WhileStatement>(s->Parent())) {
            if (stop_at_loop) {
                break;
            }
        }
    }
    return nullptr;
}

bool Validator::Atomic(const ast::Atomic* a, const sem::Atomic* s) const {
    // https://gpuweb.github.io/gpuweb/wgsl/#atomic-types
    // T must be either u32 or i32.
    if (!s->Type()->IsAnyOf<sem::U32, sem::I32>()) {
        AddError("atomic only supports i32 or u32 types", a->type ? a->type->source : a->source);
        return false;
    }
    return true;
}

bool Validator::StorageTexture(const ast::StorageTexture* t) const {
    switch (t->access) {
        case ast::Access::kWrite:
            break;
        case ast::Access::kUndefined:
            AddError("storage texture missing access control", t->source);
            return false;
        default:
            AddError("storage textures currently only support 'write' access control", t->source);
            return false;
    }

    if (!IsValidStorageTextureDimension(t->dim)) {
        AddError("cube dimensions for storage textures are not supported", t->source);
        return false;
    }

    if (!IsValidStorageTextureTexelFormat(t->format)) {
        AddError(
            "image format must be one of the texel formats specified for storage "
            "textues in https://gpuweb.github.io/gpuweb/wgsl/#texel-formats",
            t->source);
        return false;
    }
    return true;
}

bool Validator::SampledTexture(const sem::SampledTexture* t, const Source& source) const {
    if (!t->type()->UnwrapRef()->is_numeric_scalar()) {
        AddError("texture_2d<type>: type must be f32, i32 or u32", source);
        return false;
    }

    return true;
}

bool Validator::MultisampledTexture(const sem::MultisampledTexture* t, const Source& source) const {
    if (t->dim() != ast::TextureDimension::k2d) {
        AddError("only 2d multisampled textures are supported", source);
        return false;
    }

    if (!t->type()->UnwrapRef()->is_numeric_scalar()) {
        AddError("texture_multisampled_2d<type>: type must be f32, i32 or u32", source);
        return false;
    }

    return true;
}

bool Validator::Materialize(const sem::Type* to,
                            const sem::Type* from,
                            const Source& source) const {
    if (sem::Type::ConversionRank(from, to) == sem::Type::kNoConversion) {
        AddError("cannot convert value of type '" + sem_.TypeNameOf(from) + "' to type '" +
                     sem_.TypeNameOf(to) + "'",
                 source);
        return false;
    }
    return true;
}

bool Validator::VariableInitializer(const ast::Variable* v,
                                    ast::StorageClass storage_class,
                                    const sem::Type* storage_ty,
                                    const sem::Expression* initializer) const {
    auto* initializer_ty = initializer->Type();
    auto* value_type = initializer_ty->UnwrapRef();  // Implicit load of RHS

    // Value type has to match storage type
    if (storage_ty != value_type) {
        std::stringstream s;
        s << "cannot initialize " << v->Kind() << " of type '" << sem_.TypeNameOf(storage_ty)
          << "' with value of type '" << sem_.TypeNameOf(initializer_ty) << "'";
        AddError(s.str(), v->source);
        return false;
    }

    if (v->Is<ast::Var>()) {
        switch (storage_class) {
            case ast::StorageClass::kPrivate:
            case ast::StorageClass::kFunction:
                break;  // Allowed an initializer
            default:
                // https://gpuweb.github.io/gpuweb/wgsl/#var-and-let
                // Optionally has an initializer expression, if the variable is in the
                // private or function storage classes.
                AddError("var of storage class '" + utils::ToString(storage_class) +
                             "' cannot have an initializer. var initializers are only "
                             "supported for the storage classes "
                             "'private' and 'function'",
                         v->source);
                return false;
        }
    }

    return true;
}

bool Validator::StorageClassLayout(const sem::Type* store_ty,
                                   ast::StorageClass sc,
                                   Source source,
                                   ValidTypeStorageLayouts& layouts) const {
    // https://gpuweb.github.io/gpuweb/wgsl/#storage-class-layout-constraints

    auto is_uniform_struct_or_array = [sc](const sem::Type* ty) {
        return sc == ast::StorageClass::kUniform && ty->IsAnyOf<sem::Array, sem::Struct>();
    };

    auto is_uniform_struct = [sc](const sem::Type* ty) {
        return sc == ast::StorageClass::kUniform && ty->Is<sem::Struct>();
    };

    auto required_alignment_of = [&](const sem::Type* ty) {
        uint32_t actual_align = ty->Align();
        uint32_t required_align = actual_align;
        if (is_uniform_struct_or_array(ty)) {
            required_align = utils::RoundUp(16u, actual_align);
        }
        return required_align;
    };

    auto member_name_of = [this](const sem::StructMember* sm) {
        return symbols_.NameFor(sm->Declaration()->symbol);
    };

    // Cache result of type + storage class pair.
    if (!layouts.emplace(store_ty, sc).second) {
        return true;
    }

    if (!ast::IsHostShareable(sc)) {
        return true;
    }

    // Temporally forbid using f16 types in "uniform" and "storage" storage class.
    // TODO(tint:1473, tint:1502): Remove this error after f16 is supported in "uniform" and
    // "storage" storage class but keep for "push_constant" storage class.
    if (Is<sem::F16>(sem::Type::DeepestElementOf(store_ty))) {
        AddError(
            "using f16 types in '" + utils::ToString(sc) + "' storage class is not implemented yet",
            source);
        return false;
    }

    if (auto* str = store_ty->As<sem::Struct>()) {
        for (size_t i = 0; i < str->Members().size(); ++i) {
            auto* const m = str->Members()[i];
            uint32_t required_align = required_alignment_of(m->Type());

            // Recurse into the member type.
            if (!StorageClassLayout(m->Type(), sc, m->Declaration()->type->source, layouts)) {
                AddNote("see layout of struct:\n" + str->Layout(symbols_),
                        str->Declaration()->source);
                return false;
            }

            // Validate that member is at a valid byte offset
            if (m->Offset() % required_align != 0) {
                AddError("the offset of a struct member of type '" +
                             m->Type()->UnwrapRef()->FriendlyName(symbols_) +
                             "' in storage class '" + utils::ToString(sc) +
                             "' must be a multiple of " + std::to_string(required_align) +
                             " bytes, but '" + member_name_of(m) + "' is currently at offset " +
                             std::to_string(m->Offset()) + ". Consider setting @align(" +
                             std::to_string(required_align) + ") on this member",
                         m->Declaration()->source);

                AddNote("see layout of struct:\n" + str->Layout(symbols_),
                        str->Declaration()->source);

                if (auto* member_str = m->Type()->As<sem::Struct>()) {
                    AddNote("and layout of struct member:\n" + member_str->Layout(symbols_),
                            member_str->Declaration()->source);
                }

                return false;
            }

            // For uniform buffers, validate that the number of bytes between the
            // previous member of type struct and the current is a multiple of 16
            // bytes.
            auto* const prev_member = (i == 0) ? nullptr : str->Members()[i - 1];
            if (prev_member && is_uniform_struct(prev_member->Type())) {
                const uint32_t prev_to_curr_offset = m->Offset() - prev_member->Offset();
                if (prev_to_curr_offset % 16 != 0) {
                    AddError(
                        "uniform storage requires that the number of bytes between the "
                        "start of the previous member of type struct and the current "
                        "member be a multiple of 16 bytes, but there are currently " +
                            std::to_string(prev_to_curr_offset) + " bytes between '" +
                            member_name_of(prev_member) + "' and '" + member_name_of(m) +
                            "'. Consider setting @align(16) on this member",
                        m->Declaration()->source);

                    AddNote("see layout of struct:\n" + str->Layout(symbols_),
                            str->Declaration()->source);

                    auto* prev_member_str = prev_member->Type()->As<sem::Struct>();
                    AddNote("and layout of previous member struct:\n" +
                                prev_member_str->Layout(symbols_),
                            prev_member_str->Declaration()->source);
                    return false;
                }
            }
        }
    }

    // For uniform buffer array members, validate that array elements are
    // aligned to 16 bytes
    if (auto* arr = store_ty->As<sem::Array>()) {
        // Recurse into the element type.
        // TODO(crbug.com/tint/1388): Ideally we'd pass the source for nested
        // element type here, but we can't easily get that from the semantic node.
        // We should consider recursing through the AST type nodes instead.
        if (!StorageClassLayout(arr->ElemType(), sc, source, layouts)) {
            return false;
        }

        if (sc == ast::StorageClass::kUniform) {
            // We already validated that this array member is itself aligned to 16
            // bytes above, so we only need to validate that stride is a multiple
            // of 16 bytes.
            if (arr->Stride() % 16 != 0) {
                // Since WGSL has no stride attribute, try to provide a useful hint
                // for how the shader author can resolve the issue.
                std::string hint;
                if (arr->ElemType()->is_scalar()) {
                    hint =
                        "Consider using a vector or struct as the element type "
                        "instead.";
                } else if (auto* vec = arr->ElemType()->As<sem::Vector>();
                           vec && vec->type()->Size() == 4) {
                    hint = "Consider using a vec4 instead.";
                } else if (arr->ElemType()->Is<sem::Struct>()) {
                    hint =
                        "Consider using the @size attribute on the last struct "
                        "member.";
                } else {
                    hint =
                        "Consider wrapping the element type in a struct and using "
                        "the "
                        "@size attribute.";
                }
                AddError(
                    "uniform storage requires that array elements be aligned to 16 "
                    "bytes, but array element alignment is currently " +
                        std::to_string(arr->Stride()) + ". " + hint,
                    source);
                return false;
            }
        }
    }

    return true;
}

bool Validator::StorageClassLayout(const sem::Variable* var,
                                   const ast::Extensions& enabled_extensions,
                                   ValidTypeStorageLayouts& layouts) const {
    if (var->StorageClass() == ast::StorageClass::kPushConstant &&
        !enabled_extensions.Contains(ast::Extension::kChromiumExperimentalPushConstant) &&
        IsValidationEnabled(var->Declaration()->attributes,
                            ast::DisabledValidation::kIgnoreStorageClass)) {
        AddError(
            "use of variable storage class 'push_constant' requires enabling extension "
            "'chromium_experimental_push_constant'",
            var->Declaration()->source);
        return false;
    }

    if (auto* str = var->Type()->UnwrapRef()->As<sem::Struct>()) {
        if (!StorageClassLayout(str, var->StorageClass(), str->Declaration()->source, layouts)) {
            AddNote("see declaration of variable", var->Declaration()->source);
            return false;
        }
    } else {
        Source source = var->Declaration()->source;
        if (var->Declaration()->type) {
            source = var->Declaration()->type->source;
        }
        if (!StorageClassLayout(var->Type()->UnwrapRef(), var->StorageClass(), source, layouts)) {
            return false;
        }
    }

    return true;
}

bool Validator::LocalVariable(const sem::Variable* v) const {
    auto* decl = v->Declaration();
    return Switch(
        decl,  //
        [&](const ast::Var* var) {
            if (IsValidationEnabled(var->attributes,
                                    ast::DisabledValidation::kIgnoreStorageClass)) {
                if (!v->Type()->UnwrapRef()->IsConstructible()) {
                    AddError("function-scope 'var' must have a constructible type",
                             var->type ? var->type->source : var->source);
                    return false;
                }
            }
            return Var(v);
        },                                        //
        [&](const ast::Let*) { return Let(v); },  //
        [&](const ast::Const*) { return true; },  //
        [&](Default) {
            TINT_ICE(Resolver, diagnostics_)
                << "Validator::Variable() called with a unknown variable type: "
                << decl->TypeInfo().name;
            return false;
        });
}

bool Validator::GlobalVariable(
    const sem::GlobalVariable* global,
    const std::unordered_map<OverrideId, const sem::Variable*>& override_ids,
    const std::unordered_map<const sem::Type*, const Source&>& atomic_composite_info) const {
    auto* decl = global->Declaration();
    bool ok = Switch(
        decl,  //
        [&](const ast::Var* var) {
            if (auto* init = global->Constructor();
                init && init->Stage() > sem::EvaluationStage::kOverride) {
                AddError("module-scope 'var' initializer must be a constant or override expression",
                         init->Declaration()->source);
                return false;
            }

            if (global->StorageClass() == ast::StorageClass::kNone) {
                AddError("module-scope 'var' declaration must have a storage class", decl->source);
                return false;
            }

            for (auto* attr : decl->attributes) {
                bool is_shader_io_attribute =
                    attr->IsAnyOf<ast::BuiltinAttribute, ast::InterpolateAttribute,
                                  ast::InvariantAttribute, ast::LocationAttribute>();
                bool has_io_storage_class = global->StorageClass() == ast::StorageClass::kIn ||
                                            global->StorageClass() == ast::StorageClass::kOut;
                if (!attr->IsAnyOf<ast::BindingAttribute, ast::GroupAttribute,
                                   ast::InternalAttribute>() &&
                    (!is_shader_io_attribute || !has_io_storage_class)) {
                    AddError("attribute is not valid for module-scope 'var'", attr->source);
                    return false;
                }
            }

            // https://gpuweb.github.io/gpuweb/wgsl/#variable-declaration
            // The access mode always has a default, and except for variables in the
            // storage storage class, must not be written.
            if (var->declared_access != ast::Access::kUndefined) {
                if (global->StorageClass() == ast::StorageClass::kStorage) {
                    // The access mode for the storage address space can only be 'read' or
                    // 'read_write'.
                    if (var->declared_access == ast::Access::kWrite) {
                        AddError("access mode 'write' is not valid for the 'storage' address space",
                                 decl->source);
                        return false;
                    }
                } else {
                    AddError("only variables in <storage> storage class may declare an access mode",
                             decl->source);
                    return false;
                }
            }

            if (!AtomicVariable(global, atomic_composite_info)) {
                return false;
            }

            auto name = symbols_.NameFor(var->symbol);
            if (sem::ParseBuiltinType(name) != sem::BuiltinType::kNone) {
                AddError(
                    "'" + name + "' is a builtin and cannot be redeclared as a module-scope 'var'",
                    var->source);
                return false;
            }

            return Var(global);
        },
        [&](const ast::Override*) { return Override(global, override_ids); },
        [&](const ast::Const*) {
            if (!decl->attributes.IsEmpty()) {
                AddError("attribute is not valid for module-scope 'const' declaration",
                         decl->attributes[0]->source);
                return false;
            }
            return Const(global);
        },
        [&](Default) {
            TINT_ICE(Resolver, diagnostics_)
                << "Validator::GlobalVariable() called with a unknown variable type: "
                << decl->TypeInfo().name;
            return false;
        });

    if (!ok) {
        return false;
    }

    if (global->StorageClass() == ast::StorageClass::kFunction) {
        AddError("module-scope 'var' must not use storage class 'function'", decl->source);
        return false;
    }

    switch (global->StorageClass()) {
        case ast::StorageClass::kUniform:
        case ast::StorageClass::kStorage:
        case ast::StorageClass::kHandle: {
            // https://gpuweb.github.io/gpuweb/wgsl/#resource-interface
            // Each resource variable must be declared with both group and binding
            // attributes.
            if (!decl->HasBindingPoint()) {
                AddError("resource variables require @group and @binding attributes", decl->source);
                return false;
            }
            break;
        }
        default: {
            auto* binding_attr = ast::GetAttribute<ast::BindingAttribute>(decl->attributes);
            auto* group_attr = ast::GetAttribute<ast::GroupAttribute>(decl->attributes);
            if (binding_attr || group_attr) {
                // https://gpuweb.github.io/gpuweb/wgsl/#attribute-binding
                // Must only be applied to a resource variable
                AddError("non-resource variables must not have @group or @binding attributes",
                         decl->source);
                return false;
            }
        }
    }

    return true;
}

// https://gpuweb.github.io/gpuweb/wgsl/#atomic-types
// Atomic types may only be instantiated by variables in the workgroup storage
// class or by storage buffer variables with a read_write access mode.
bool Validator::AtomicVariable(
    const sem::Variable* var,
    std::unordered_map<const sem::Type*, const Source&> atomic_composite_info) const {
    auto sc = var->StorageClass();
    auto* decl = var->Declaration();
    auto access = var->Access();
    auto* type = var->Type()->UnwrapRef();
    auto source = decl->type ? decl->type->source : decl->source;

    if (type->Is<sem::Atomic>()) {
        if (sc != ast::StorageClass::kWorkgroup && sc != ast::StorageClass::kStorage) {
            AddError("atomic variables must have <storage> or <workgroup> storage class", source);
            return false;
        }
    } else if (type->IsAnyOf<sem::Struct, sem::Array>()) {
        auto found = atomic_composite_info.find(type);
        if (found != atomic_composite_info.end()) {
            if (sc != ast::StorageClass::kStorage && sc != ast::StorageClass::kWorkgroup) {
                AddError("atomic variables must have <storage> or <workgroup> storage class",
                         source);
                AddNote("atomic sub-type of '" + sem_.TypeNameOf(type) + "' is declared here",
                        found->second);
                return false;
            } else if (sc == ast::StorageClass::kStorage && access != ast::Access::kReadWrite) {
                AddError(
                    "atomic variables in <storage> storage class must have read_write "
                    "access mode",
                    source);
                AddNote("atomic sub-type of '" + sem_.TypeNameOf(type) + "' is declared here",
                        found->second);
                return false;
            }
        }
    }

    return true;
}

bool Validator::Var(const sem::Variable* v) const {
    auto* var = v->Declaration()->As<ast::Var>();
    auto* storage_ty = v->Type()->UnwrapRef();

    if (!IsStorable(storage_ty)) {
        AddError(sem_.TypeNameOf(storage_ty) + " cannot be used as the type of a var", var->source);
        return false;
    }

    if (storage_ty->is_handle() && var->declared_storage_class != ast::StorageClass::kNone) {
        // https://gpuweb.github.io/gpuweb/wgsl/#module-scope-variables
        // If the store type is a texture type or a sampler type, then the
        // variable declaration must not have a storage class attribute. The
        // storage class will always be handle.
        AddError(
            "variables of type '" + sem_.TypeNameOf(storage_ty) + "' must not have a storage class",
            var->source);
        return false;
    }

    if (IsValidationEnabled(var->attributes, ast::DisabledValidation::kIgnoreStorageClass) &&
        (var->declared_storage_class == ast::StorageClass::kIn ||
         var->declared_storage_class == ast::StorageClass::kOut)) {
        AddError("invalid use of input/output storage class", var->source);
        return false;
    }
    return true;
}

bool Validator::Let(const sem::Variable* v) const {
    auto* decl = v->Declaration();
    auto* storage_ty = v->Type()->UnwrapRef();

    if (!(storage_ty->IsConstructible() || storage_ty->Is<sem::Pointer>())) {
        AddError(sem_.TypeNameOf(storage_ty) + " cannot be used as the type of a 'let'",
                 decl->source);
        return false;
    }
    return true;
}

bool Validator::Override(
    const sem::Variable* v,
    const std::unordered_map<OverrideId, const sem::Variable*>& override_ids) const {
    auto* decl = v->Declaration();
    auto* storage_ty = v->Type()->UnwrapRef();

    if (auto* init = v->Constructor(); init && init->Stage() > sem::EvaluationStage::kOverride) {
        AddError("'override' initializer must be an override expression",
                 init->Declaration()->source);
        return false;
    }

    for (auto* attr : decl->attributes) {
        if (auto* id_attr = attr->As<ast::IdAttribute>()) {
            uint32_t id = id_attr->value;
            if (id > std::numeric_limits<decltype(OverrideId::value)>::max()) {
                AddError(
                    "override IDs must be between 0 and " +
                        std::to_string(std::numeric_limits<decltype(OverrideId::value)>::max()),
                    attr->source);
                return false;
            }
            if (auto it =
                    override_ids.find(OverrideId{static_cast<decltype(OverrideId::value)>(id)});
                it != override_ids.end() && it->second != v) {
                AddError("override IDs must be unique", attr->source);
                AddNote("a override with an ID of " + std::to_string(id) +
                            " was previously declared here:",
                        ast::GetAttribute<ast::IdAttribute>(it->second->Declaration()->attributes)
                            ->source);
                return false;
            }
        } else {
            AddError("attribute is not valid for 'override' declaration", attr->source);
            return false;
        }
    }

    auto name = symbols_.NameFor(decl->symbol);
    if (sem::ParseBuiltinType(name) != sem::BuiltinType::kNone) {
        AddError("'" + name + "' is a builtin and cannot be redeclared as a 'override'",
                 decl->source);
        return false;
    }

    if (!storage_ty->is_scalar()) {
        AddError(sem_.TypeNameOf(storage_ty) + " cannot be used as the type of a 'override'",
                 decl->source);
        return false;
    }

    if (storage_ty->Is<sem::F16>()) {
        AddError("'override' of type f16 is not implemented yet", decl->source);
        return false;
    }

    return true;
}

bool Validator::Const(const sem::Variable* v) const {
    auto* decl = v->Declaration();

    auto name = symbols_.NameFor(decl->symbol);
    if (sem::ParseBuiltinType(name) != sem::BuiltinType::kNone) {
        AddError("'" + name + "' is a builtin and cannot be redeclared as a 'const'", decl->source);
        return false;
    }

    return true;
}

bool Validator::Parameter(const ast::Function* func, const sem::Variable* var) const {
    auto* decl = var->Declaration();

    if (IsValidationDisabled(decl->attributes, ast::DisabledValidation::kFunctionParameter)) {
        return true;
    }

    for (auto* attr : decl->attributes) {
        if (!func->IsEntryPoint() && !attr->Is<ast::InternalAttribute>()) {
            AddError("attribute is not valid for non-entry point function parameters",
                     attr->source);
            return false;
        }
        if (!attr->IsAnyOf<ast::BuiltinAttribute, ast::InvariantAttribute, ast::LocationAttribute,
                           ast::InterpolateAttribute, ast::InternalAttribute>() &&
            (IsValidationEnabled(decl->attributes,
                                 ast::DisabledValidation::kEntryPointParameter))) {
            AddError("attribute is not valid for function parameters", attr->source);
            return false;
        }
    }

    if (auto* ref = var->Type()->As<sem::Pointer>()) {
        auto sc = ref->StorageClass();
        if (!(sc == ast::StorageClass::kFunction || sc == ast::StorageClass::kPrivate ||
              sc == ast::StorageClass::kWorkgroup) &&
            IsValidationEnabled(decl->attributes, ast::DisabledValidation::kIgnoreStorageClass)) {
            std::stringstream ss;
            ss << "function parameter of pointer type cannot be in '" << sc << "' storage class";
            AddError(ss.str(), decl->source);
            return false;
        }
    }

    if (IsPlain(var->Type())) {
        if (!var->Type()->IsConstructible()) {
            AddError("type of function parameter must be constructible", decl->source);
            return false;
        }
    } else if (!var->Type()->IsAnyOf<sem::Texture, sem::Sampler, sem::Pointer>()) {
        AddError("type of function parameter cannot be " + sem_.TypeNameOf(var->Type()),
                 decl->source);
        return false;
    }

    return true;
}

bool Validator::BuiltinAttribute(const ast::BuiltinAttribute* attr,
                                 const sem::Type* storage_ty,
                                 ast::PipelineStage stage,
                                 const bool is_input) const {
    auto* type = storage_ty->UnwrapRef();
    std::stringstream stage_name;
    stage_name << stage;
    bool is_stage_mismatch = false;
    bool is_output = !is_input;
    switch (attr->builtin) {
        case ast::BuiltinValue::kPosition:
            if (stage != ast::PipelineStage::kNone &&
                !((is_input && stage == ast::PipelineStage::kFragment) ||
                  (is_output && stage == ast::PipelineStage::kVertex))) {
                is_stage_mismatch = true;
            }
            if (!(type->is_float_vector() && type->As<sem::Vector>()->Width() == 4)) {
                AddError("store type of " + attr_to_str(attr) + " must be 'vec4<f32>'",
                         attr->source);
                return false;
            }
            break;
        case ast::BuiltinValue::kGlobalInvocationId:
        case ast::BuiltinValue::kLocalInvocationId:
        case ast::BuiltinValue::kNumWorkgroups:
        case ast::BuiltinValue::kWorkgroupId:
            if (stage != ast::PipelineStage::kNone &&
                !(stage == ast::PipelineStage::kCompute && is_input)) {
                is_stage_mismatch = true;
            }
            if (!(type->is_unsigned_integer_vector() && type->As<sem::Vector>()->Width() == 3)) {
                AddError("store type of " + attr_to_str(attr) + " must be 'vec3<u32>'",
                         attr->source);
                return false;
            }
            break;
        case ast::BuiltinValue::kFragDepth:
            if (stage != ast::PipelineStage::kNone &&
                !(stage == ast::PipelineStage::kFragment && !is_input)) {
                is_stage_mismatch = true;
            }
            if (!type->Is<sem::F32>()) {
                AddError("store type of " + attr_to_str(attr) + " must be 'f32'", attr->source);
                return false;
            }
            break;
        case ast::BuiltinValue::kFrontFacing:
            if (stage != ast::PipelineStage::kNone &&
                !(stage == ast::PipelineStage::kFragment && is_input)) {
                is_stage_mismatch = true;
            }
            if (!type->Is<sem::Bool>()) {
                AddError("store type of " + attr_to_str(attr) + " must be 'bool'", attr->source);
                return false;
            }
            break;
        case ast::BuiltinValue::kLocalInvocationIndex:
            if (stage != ast::PipelineStage::kNone &&
                !(stage == ast::PipelineStage::kCompute && is_input)) {
                is_stage_mismatch = true;
            }
            if (!type->Is<sem::U32>()) {
                AddError("store type of " + attr_to_str(attr) + " must be 'u32'", attr->source);
                return false;
            }
            break;
        case ast::BuiltinValue::kVertexIndex:
        case ast::BuiltinValue::kInstanceIndex:
            if (stage != ast::PipelineStage::kNone &&
                !(stage == ast::PipelineStage::kVertex && is_input)) {
                is_stage_mismatch = true;
            }
            if (!type->Is<sem::U32>()) {
                AddError("store type of " + attr_to_str(attr) + " must be 'u32'", attr->source);
                return false;
            }
            break;
        case ast::BuiltinValue::kSampleMask:
            if (stage != ast::PipelineStage::kNone && !(stage == ast::PipelineStage::kFragment)) {
                is_stage_mismatch = true;
            }
            if (!type->Is<sem::U32>()) {
                AddError("store type of " + attr_to_str(attr) + " must be 'u32'", attr->source);
                return false;
            }
            break;
        case ast::BuiltinValue::kSampleIndex:
            if (stage != ast::PipelineStage::kNone &&
                !(stage == ast::PipelineStage::kFragment && is_input)) {
                is_stage_mismatch = true;
            }
            if (!type->Is<sem::U32>()) {
                AddError("store type of " + attr_to_str(attr) + " must be 'u32'", attr->source);
                return false;
            }
            break;
        default:
            break;
    }

    if (is_stage_mismatch) {
        AddError(attr_to_str(attr) + " cannot be used in " +
                     (is_input ? "input of " : "output of ") + stage_name.str() + " pipeline stage",
                 attr->source);
        return false;
    }

    return true;
}

bool Validator::InterpolateAttribute(const ast::InterpolateAttribute* attr,
                                     const sem::Type* storage_ty) const {
    auto* type = storage_ty->UnwrapRef();

    if (type->is_integer_scalar_or_vector() && attr->type != ast::InterpolationType::kFlat) {
        AddError("interpolation type must be 'flat' for integral user-defined IO types",
                 attr->source);
        return false;
    }

    if (attr->type == ast::InterpolationType::kFlat &&
        attr->sampling != ast::InterpolationSampling::kNone) {
        AddError("flat interpolation attribute must not have a sampling parameter", attr->source);
        return false;
    }

    return true;
}

bool Validator::Function(const sem::Function* func, ast::PipelineStage stage) const {
    auto* decl = func->Declaration();

    auto name = symbols_.NameFor(decl->symbol);
    if (sem::ParseBuiltinType(name) != sem::BuiltinType::kNone) {
        AddError("'" + name + "' is a builtin and cannot be redeclared as a function",
                 decl->source);
        return false;
    }

    for (auto* attr : decl->attributes) {
        if (attr->Is<ast::WorkgroupAttribute>()) {
            if (decl->PipelineStage() != ast::PipelineStage::kCompute) {
                AddError("the workgroup_size attribute is only valid for compute stages",
                         attr->source);
                return false;
            }
        } else if (!attr->IsAnyOf<ast::StageAttribute, ast::InternalAttribute>()) {
            AddError("attribute is not valid for functions", attr->source);
            return false;
        }
    }

    if (decl->params.Length() > 255) {
        AddError("functions may declare at most 255 parameters", decl->source);
        return false;
    }

    if (!func->ReturnType()->Is<sem::Void>()) {
        if (!func->ReturnType()->IsConstructible()) {
            AddError("function return type must be a constructible type",
                     decl->return_type->source);
            return false;
        }

        if (decl->body) {
            sem::Behaviors behaviors{sem::Behavior::kNext};
            if (auto* last = decl->body->Last()) {
                behaviors = sem_.Get(last)->Behaviors();
            }
            if (behaviors.Contains(sem::Behavior::kNext)) {
                AddError("missing return at end of function", decl->source);
                return false;
            }
        } else if (IsValidationEnabled(decl->attributes,
                                       ast::DisabledValidation::kFunctionHasNoBody)) {
            TINT_ICE(Resolver, diagnostics_)
                << "Function " << symbols_.NameFor(decl->symbol) << " has no body";
        }

        for (auto* attr : decl->return_type_attributes) {
            if (!decl->IsEntryPoint()) {
                AddError("attribute is not valid for non-entry point function return types",
                         attr->source);
                return false;
            }
            if (!attr->IsAnyOf<ast::BuiltinAttribute, ast::InternalAttribute,
                               ast::LocationAttribute, ast::InterpolateAttribute,
                               ast::InvariantAttribute>() &&
                (IsValidationEnabled(decl->attributes,
                                     ast::DisabledValidation::kEntryPointParameter) &&
                 IsValidationEnabled(decl->attributes,
                                     ast::DisabledValidation::kFunctionParameter))) {
                AddError("attribute is not valid for entry point return types", attr->source);
                return false;
            }
        }
    }

    if (decl->IsEntryPoint()) {
        if (!EntryPoint(func, stage)) {
            return false;
        }
    }

    // https://www.w3.org/TR/WGSL/#behaviors-rules
    // a function behavior is always one of {}, {Next}, {Discard}, or
    // {Next, Discard}.
    if (func->Behaviors() != sem::Behaviors{} &&  // NOLINT: bad warning
        func->Behaviors() != sem::Behavior::kNext && func->Behaviors() != sem::Behavior::kDiscard &&
        func->Behaviors() != sem::Behaviors{sem::Behavior::kNext,  //
                                            sem::Behavior::kDiscard}) {
        TINT_ICE(Resolver, diagnostics_)
            << "function '" << name << "' behaviors are: " << func->Behaviors();
    }

    return true;
}

bool Validator::EntryPoint(const sem::Function* func, ast::PipelineStage stage) const {
    auto* decl = func->Declaration();

    // Use a lambda to validate the entry point attributes for a type.
    // Persistent state is used to track which builtins and locations have
    // already been seen, in order to catch conflicts.
    // TODO(jrprice): This state could be stored in sem::Function instead, and
    // then passed to sem::Function since it would be useful there too.
    std::unordered_set<ast::BuiltinValue> builtins;
    std::unordered_set<uint32_t> locations;
    enum class ParamOrRetType {
        kParameter,
        kReturnType,
    };

    // Inner lambda that is applied to a type and all of its members.
    auto validate_entry_point_attributes_inner = [&](utils::VectorRef<const ast::Attribute*> attrs,
                                                     const sem::Type* ty, Source source,
                                                     ParamOrRetType param_or_ret,
                                                     bool is_struct_member) {
        // Temporally forbid using f16 types in entry point IO.
        // TODO(tint:1473, tint:1502): Remove this error after f16 is supported in entry point
        // IO.
        if (Is<sem::F16>(sem::Type::DeepestElementOf(ty))) {
            AddError("entry point IO of f16 types is not implemented yet", source);
            return false;
        }

        // Scan attributes for pipeline IO attributes.
        // Check for overlap with attributes that have been seen previously.
        const ast::Attribute* pipeline_io_attribute = nullptr;
        const ast::InterpolateAttribute* interpolate_attribute = nullptr;
        const ast::InvariantAttribute* invariant_attribute = nullptr;
        for (auto* attr : attrs) {
            auto is_invalid_compute_shader_attribute = false;

            if (auto* builtin = attr->As<ast::BuiltinAttribute>()) {
                if (pipeline_io_attribute) {
                    AddError("multiple entry point IO attributes", attr->source);
                    AddNote("previously consumed " + attr_to_str(pipeline_io_attribute),
                            pipeline_io_attribute->source);
                    return false;
                }
                pipeline_io_attribute = attr;

                if (builtins.count(builtin->builtin)) {
                    AddError(attr_to_str(builtin) +
                                 " attribute appears multiple times as pipeline " +
                                 (param_or_ret == ParamOrRetType::kParameter ? "input" : "output"),
                             decl->source);
                    return false;
                }

                if (!BuiltinAttribute(builtin, ty, stage,
                                      /* is_input */ param_or_ret == ParamOrRetType::kParameter)) {
                    return false;
                }
                builtins.emplace(builtin->builtin);
            } else if (auto* location = attr->As<ast::LocationAttribute>()) {
                if (pipeline_io_attribute) {
                    AddError("multiple entry point IO attributes", attr->source);
                    AddNote("previously consumed " + attr_to_str(pipeline_io_attribute),
                            pipeline_io_attribute->source);
                    return false;
                }
                pipeline_io_attribute = attr;

                bool is_input = param_or_ret == ParamOrRetType::kParameter;

                if (!LocationAttribute(location, ty, locations, stage, source, is_input)) {
                    return false;
                }
            } else if (auto* interpolate = attr->As<ast::InterpolateAttribute>()) {
                if (decl->PipelineStage() == ast::PipelineStage::kCompute) {
                    is_invalid_compute_shader_attribute = true;
                } else if (!InterpolateAttribute(interpolate, ty)) {
                    return false;
                }
                interpolate_attribute = interpolate;
            } else if (auto* invariant = attr->As<ast::InvariantAttribute>()) {
                if (decl->PipelineStage() == ast::PipelineStage::kCompute) {
                    is_invalid_compute_shader_attribute = true;
                }
                invariant_attribute = invariant;
            }
            if (is_invalid_compute_shader_attribute) {
                std::string input_or_output =
                    param_or_ret == ParamOrRetType::kParameter ? "inputs" : "output";
                AddError("attribute is not valid for compute shader " + input_or_output,
                         attr->source);
                return false;
            }
        }

        if (IsValidationEnabled(attrs, ast::DisabledValidation::kEntryPointParameter)) {
            if (is_struct_member && ty->Is<sem::Struct>()) {
                AddError("nested structures cannot be used for entry point IO", source);
                return false;
            }

            if (!ty->Is<sem::Struct>() && !pipeline_io_attribute) {
                std::string err = "missing entry point IO attribute";
                if (!is_struct_member) {
                    err += (param_or_ret == ParamOrRetType::kParameter ? " on parameter"
                                                                       : " on return type");
                }
                AddError(err, source);
                return false;
            }

            if (pipeline_io_attribute && pipeline_io_attribute->Is<ast::LocationAttribute>()) {
                if (ty->is_integer_scalar_or_vector() && !interpolate_attribute) {
                    if (decl->PipelineStage() == ast::PipelineStage::kVertex &&
                        param_or_ret == ParamOrRetType::kReturnType) {
                        AddError(
                            "integral user-defined vertex outputs must have a flat "
                            "interpolation attribute",
                            source);
                        return false;
                    }
                    if (decl->PipelineStage() == ast::PipelineStage::kFragment &&
                        param_or_ret == ParamOrRetType::kParameter) {
                        AddError(
                            "integral user-defined fragment inputs must have a flat "
                            "interpolation attribute",
                            source);
                        return false;
                    }
                }
            }

            if (interpolate_attribute) {
                if (!pipeline_io_attribute ||
                    !pipeline_io_attribute->Is<ast::LocationAttribute>()) {
                    AddError("interpolate attribute must only be used with @location",
                             interpolate_attribute->source);
                    return false;
                }
            }

            if (invariant_attribute) {
                bool has_position = false;
                if (pipeline_io_attribute) {
                    if (auto* builtin = pipeline_io_attribute->As<ast::BuiltinAttribute>()) {
                        has_position = (builtin->builtin == ast::BuiltinValue::kPosition);
                    }
                }
                if (!has_position) {
                    AddError(
                        "invariant attribute must only be applied to a position "
                        "builtin",
                        invariant_attribute->source);
                    return false;
                }
            }
        }
        return true;
    };

    // Outer lambda for validating the entry point attributes for a type.
    auto validate_entry_point_attributes = [&](utils::VectorRef<const ast::Attribute*> attrs,
                                               const sem::Type* ty, Source source,
                                               ParamOrRetType param_or_ret) {
        if (!validate_entry_point_attributes_inner(attrs, ty, source, param_or_ret,
                                                   /*is_struct_member*/ false)) {
            return false;
        }

        if (auto* str = ty->As<sem::Struct>()) {
            for (auto* member : str->Members()) {
                if (!validate_entry_point_attributes_inner(
                        member->Declaration()->attributes, member->Type(),
                        member->Declaration()->source, param_or_ret,
                        /*is_struct_member*/ true)) {
                    AddNote("while analysing entry point '" + symbols_.NameFor(decl->symbol) + "'",
                            decl->source);
                    return false;
                }
            }
        }

        return true;
    };

    for (auto* param : func->Parameters()) {
        auto* param_decl = param->Declaration();
        if (!validate_entry_point_attributes(param_decl->attributes, param->Type(),
                                             param_decl->source, ParamOrRetType::kParameter)) {
            return false;
        }
    }

    // Clear IO sets after parameter validation. Builtin and location attributes
    // in return types should be validated independently from those used in
    // parameters.
    builtins.clear();
    locations.clear();

    if (!func->ReturnType()->Is<sem::Void>()) {
        if (!validate_entry_point_attributes(decl->return_type_attributes, func->ReturnType(),
                                             decl->source, ParamOrRetType::kReturnType)) {
            return false;
        }
    }

    if (decl->PipelineStage() == ast::PipelineStage::kVertex &&
        builtins.count(ast::BuiltinValue::kPosition) == 0) {
        // Check module-scope variables, as the SPIR-V sanitizer generates these.
        bool found = false;
        for (auto* global : func->TransitivelyReferencedGlobals()) {
            if (auto* builtin =
                    ast::GetAttribute<ast::BuiltinAttribute>(global->Declaration()->attributes)) {
                if (builtin->builtin == ast::BuiltinValue::kPosition) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            AddError(
                "a vertex shader must include the 'position' builtin in its return "
                "type",
                decl->source);
            return false;
        }
    }

    if (decl->PipelineStage() == ast::PipelineStage::kCompute) {
        if (!ast::HasAttribute<ast::WorkgroupAttribute>(decl->attributes)) {
            AddError(
                "a compute shader must include 'workgroup_size' in its "
                "attributes",
                decl->source);
            return false;
        }
    }

    // Validate there are no resource variable binding collisions
    std::unordered_map<sem::BindingPoint, const ast::Variable*> binding_points;
    for (auto* global : func->TransitivelyReferencedGlobals()) {
        auto* var_decl = global->Declaration()->As<ast::Var>();
        if (!var_decl || !var_decl->HasBindingPoint()) {
            continue;
        }
        auto bp = global->BindingPoint();
        auto res = binding_points.emplace(bp, var_decl);
        if (!res.second &&
            IsValidationEnabled(decl->attributes,
                                ast::DisabledValidation::kBindingPointCollision) &&
            IsValidationEnabled(res.first->second->attributes,
                                ast::DisabledValidation::kBindingPointCollision)) {
            // https://gpuweb.github.io/gpuweb/wgsl/#resource-interface
            // Bindings must not alias within a shader stage: two different
            // variables in the resource interface of a given shader must not have
            // the same group and binding values, when considered as a pair of
            // values.
            auto func_name = symbols_.NameFor(decl->symbol);
            AddError("entry point '" + func_name +
                         "' references multiple variables that use the "
                         "same resource binding @group(" +
                         std::to_string(bp.group) + "), @binding(" + std::to_string(bp.binding) +
                         ")",
                     var_decl->source);
            AddNote("first resource binding usage declared here", res.first->second->source);
            return false;
        }
    }

    return true;
}

bool Validator::Statements(utils::VectorRef<const ast::Statement*> stmts) const {
    for (auto* stmt : stmts) {
        if (!sem_.Get(stmt)->IsReachable()) {
            /// TODO(https://github.com/gpuweb/gpuweb/issues/2378): This may need to
            /// become an error.
            AddWarning("code is unreachable", stmt->source);
            break;
        }
    }
    return true;
}

bool Validator::Bitcast(const ast::BitcastExpression* cast, const sem::Type* to) const {
    auto* from = sem_.TypeOf(cast->expr)->UnwrapRef();
    if (!from->is_numeric_scalar_or_vector()) {
        AddError("'" + sem_.TypeNameOf(from) + "' cannot be bitcast", cast->expr->source);
        return false;
    }
    if (!to->is_numeric_scalar_or_vector()) {
        AddError("cannot bitcast to '" + sem_.TypeNameOf(to) + "'", cast->type->source);
        return false;
    }

    auto width = [&](const sem::Type* ty) {
        if (auto* vec = ty->As<sem::Vector>()) {
            return vec->Width();
        }
        return 1u;
    };

    if (width(from) != width(to)) {
        AddError(
            "cannot bitcast from '" + sem_.TypeNameOf(from) + "' to '" + sem_.TypeNameOf(to) + "'",
            cast->source);
        return false;
    }

    return true;
}

bool Validator::BreakStatement(const sem::Statement* stmt,
                               sem::Statement* current_statement) const {
    if (!stmt->FindFirstParent<sem::LoopBlockStatement, sem::CaseStatement>()) {
        AddError("break statement must be in a loop or switch case", stmt->Declaration()->source);
        return false;
    }
    if (auto* continuing = ClosestContinuing(/*stop_at_loop*/ true, current_statement)) {
        auto fail = [&](const char* note_msg, const Source& note_src) {
            constexpr const char* kErrorMsg =
                "break statement in a continuing block must be the single statement "
                "of an if statement's true or false block, and that if statement "
                "must be the last statement of the continuing block";
            AddError(kErrorMsg, stmt->Declaration()->source);
            AddNote(note_msg, note_src);
            return false;
        };

        if (auto* block = stmt->Parent()->As<sem::BlockStatement>()) {
            auto* block_parent = block->Parent();
            auto* if_stmt = block_parent->As<sem::IfStatement>();
            if (!if_stmt) {
                return fail("break statement is not directly in if statement block",
                            stmt->Declaration()->source);
            }
            if (block->Declaration()->statements.Length() != 1) {
                return fail("if statement block contains multiple statements",
                            block->Declaration()->source);
            }

            if (if_stmt->Parent()->Is<sem::IfStatement>()) {
                return fail("else has condition", if_stmt->Declaration()->source);
            }

            bool el_contains_break = block->Declaration() == if_stmt->Declaration()->else_statement;
            if (el_contains_break) {
                if (auto* true_block = if_stmt->Declaration()->body; !true_block->Empty()) {
                    return fail("non-empty true block", true_block->source);
                }
            } else {
                auto* else_stmt = if_stmt->Declaration()->else_statement;
                if (else_stmt) {
                    return fail("non-empty false block", else_stmt->source);
                }
            }

            if (if_stmt->Parent()->Declaration() != continuing) {
                return fail(
                    "if statement containing break statement is not directly in "
                    "continuing block",
                    if_stmt->Declaration()->source);
            }
            if (auto* cont_block = continuing->As<ast::BlockStatement>()) {
                if (if_stmt->Declaration() != cont_block->Last()) {
                    return fail(
                        "if statement containing break statement is not the last "
                        "statement of the continuing block",
                        if_stmt->Declaration()->source);
                }
            }
        }
    }
    return true;
}

bool Validator::ContinueStatement(const sem::Statement* stmt,
                                  sem::Statement* current_statement) const {
    if (auto* continuing = ClosestContinuing(/*stop_at_loop*/ true, current_statement)) {
        AddError("continuing blocks must not contain a continue statement",
                 stmt->Declaration()->source);
        if (continuing != stmt->Declaration() && continuing != stmt->Parent()->Declaration()) {
            AddNote("see continuing block here", continuing->source);
        }
        return false;
    }

    if (!stmt->FindFirstParent<sem::LoopBlockStatement>()) {
        AddError("continue statement must be in a loop", stmt->Declaration()->source);
        return false;
    }

    return true;
}

bool Validator::Call(const sem::Call* call, sem::Statement* current_statement) const {
    auto* expr = call->Declaration();
    bool is_call_stmt =
        current_statement && Is<ast::CallStatement>(current_statement->Declaration(),
                                                    [&](auto* stmt) { return stmt->expr == expr; });
    if (is_call_stmt) {
        return Switch(
            call->Target(),  //
            [&](const sem::TypeConversion*) {
                AddError("type conversion evaluated but not used", call->Declaration()->source);
                return false;
            },
            [&](const sem::TypeConstructor*) {
                AddError("type constructor evaluated but not used", call->Declaration()->source);
                return false;
            },
            [&](Default) { return true; });
    }

    return true;
}

bool Validator::DiscardStatement(const sem::Statement* stmt,
                                 sem::Statement* current_statement) const {
    if (auto* continuing = ClosestContinuing(/*stop_at_loop*/ false, current_statement)) {
        AddError("continuing blocks must not contain a discard statement",
                 stmt->Declaration()->source);
        if (continuing != stmt->Declaration() && continuing != stmt->Parent()->Declaration()) {
            AddNote("see continuing block here", continuing->source);
        }
        return false;
    }
    return true;
}

bool Validator::FallthroughStatement(const sem::Statement* stmt) const {
    if (auto* block = As<sem::BlockStatement>(stmt->Parent())) {
        if (auto* c = As<sem::CaseStatement>(block->Parent())) {
            if (block->Declaration()->Last() == stmt->Declaration()) {
                if (auto* s = As<sem::SwitchStatement>(c->Parent())) {
                    if (c->Declaration() != s->Declaration()->body.Back()) {
                        return true;
                    }
                    AddError(
                        "a fallthrough statement must not be used in the last switch "
                        "case",
                        stmt->Declaration()->source);
                    return false;
                }
            }
        }
    }
    AddError("fallthrough must only be used as the last statement of a case block",
             stmt->Declaration()->source);
    return false;
}

bool Validator::LoopStatement(const sem::LoopStatement* stmt) const {
    if (stmt->Behaviors().Empty()) {
        AddError("loop does not exit", stmt->Declaration()->source.Begin());
        return false;
    }
    return true;
}

bool Validator::ForLoopStatement(const sem::ForLoopStatement* stmt) const {
    if (stmt->Behaviors().Empty()) {
        AddError("for-loop does not exit", stmt->Declaration()->source.Begin());
        return false;
    }
    if (auto* cond = stmt->Condition()) {
        auto* cond_ty = cond->Type()->UnwrapRef();
        if (!cond_ty->Is<sem::Bool>()) {
            AddError("for-loop condition must be bool, got " + sem_.TypeNameOf(cond_ty),
                     stmt->Condition()->Declaration()->source);
            return false;
        }
    }
    return true;
}

bool Validator::WhileStatement(const sem::WhileStatement* stmt) const {
    if (stmt->Behaviors().Empty()) {
        AddError("while does not exit", stmt->Declaration()->source.Begin());
        return false;
    }
    if (auto* cond = stmt->Condition()) {
        auto* cond_ty = cond->Type()->UnwrapRef();
        if (!cond_ty->Is<sem::Bool>()) {
            AddError("while condition must be bool, got " + sem_.TypeNameOf(cond_ty),
                     stmt->Condition()->Declaration()->source);
            return false;
        }
    }
    return true;
}

bool Validator::IfStatement(const sem::IfStatement* stmt) const {
    auto* cond_ty = stmt->Condition()->Type()->UnwrapRef();
    if (!cond_ty->Is<sem::Bool>()) {
        AddError("if statement condition must be bool, got " + sem_.TypeNameOf(cond_ty),
                 stmt->Condition()->Declaration()->source);
        return false;
    }
    return true;
}

bool Validator::BuiltinCall(const sem::Call* call) const {
    if (call->Type()->Is<sem::Void>()) {
        bool is_call_statement = false;
        // Some built-in call are not owned by a statement, e.g. a built-in called in global
        // variable declaration. Calling no-return-value built-in in these context is invalid as
        // well.
        if (auto* call_stmt = call->Stmt()) {
            if (auto* call_stmt_ast = As<ast::CallStatement>(call_stmt->Declaration())) {
                if (call_stmt_ast->expr == call->Declaration()) {
                    is_call_statement = true;
                }
            }
        }
        if (!is_call_statement) {
            // https://gpuweb.github.io/gpuweb/wgsl/#function-call-expr
            // If the called function does not return a value, a function call
            // statement should be used instead.
            auto* ident = call->Declaration()->target.name;
            auto name = symbols_.NameFor(ident->symbol);
            AddError("builtin '" + name + "' does not return a value", call->Declaration()->source);
            return false;
        }
    }

    return true;
}

bool Validator::TextureBuiltinFunction(const sem::Call* call) const {
    auto* builtin = call->Target()->As<sem::Builtin>();
    if (!builtin) {
        return false;
    }

    std::string func_name = builtin->str();
    auto& signature = builtin->Signature();

    auto check_arg_is_constexpr = [&](sem::ParameterUsage usage, int min, int max) {
        auto signed_index = signature.IndexOf(usage);
        if (signed_index < 0) {
            return true;
        }
        auto index = static_cast<size_t>(signed_index);
        std::string name = sem::str(usage);
        auto* arg = call->Arguments()[index];
        if (auto values = arg->ConstantValue()) {
            // Assert that the constant values are of the expected type.
            if (!values->Type()->is_integer_scalar_or_vector()) {
                TINT_ICE(Resolver, diagnostics_)
                    << "failed to resolve '" + func_name + "' " << name << " parameter type";
                return false;
            }

            // Currently const_expr is restricted to literals and type constructors.
            // Check that that's all we have for the parameter.
            bool is_const_expr = true;
            ast::TraverseExpressions(
                arg->Declaration(), diagnostics_, [&](const ast::Expression* e) {
                    if (e->IsAnyOf<ast::LiteralExpression, ast::CallExpression>()) {
                        return ast::TraverseAction::Descend;
                    }
                    is_const_expr = false;
                    return ast::TraverseAction::Stop;
                });
            if (is_const_expr) {
                if (auto* vector = builtin->Parameters()[index]->Type()->As<sem::Vector>()) {
                    for (size_t i = 0; i < vector->Width(); i++) {
                        auto value = values->Index(i)->As<AInt>();
                        if (value < min || value > max) {
                            AddError("each component of the " + name +
                                         " argument must be at least " + std::to_string(min) +
                                         " and at most " + std::to_string(max) + ". " + name +
                                         " component " + std::to_string(i) + " is " +
                                         std::to_string(value),
                                     arg->Declaration()->source);
                            return false;
                        }
                    }
                } else {
                    auto value = values->As<AInt>();
                    if (value < min || value > max) {
                        AddError("the " + name + " argument must be at least " +
                                     std::to_string(min) + " and at most " + std::to_string(max) +
                                     ". " + name + " is " + std::to_string(value),
                                 arg->Declaration()->source);
                        return false;
                    }
                }
                return true;
            }
        }
        AddError("the " + name + " argument must be a const_expression",
                 arg->Declaration()->source);
        return false;
    };

    return check_arg_is_constexpr(sem::ParameterUsage::kOffset, -8, 7) &&
           check_arg_is_constexpr(sem::ParameterUsage::kComponent, 0, 3);
}

bool Validator::RequiredExtensionForBuiltinFunction(
    const sem::Call* call,
    const ast::Extensions& enabled_extensions) const {
    const auto* builtin = call->Target()->As<sem::Builtin>();
    if (!builtin) {
        return true;
    }

    const auto extension = builtin->RequiredExtension();
    if (extension == ast::Extension::kInvalid) {
        return true;
    }

    if (!enabled_extensions.Contains(extension)) {
        AddError("cannot call built-in function '" + std::string(builtin->str()) +
                     "' without extension " + utils::ToString(extension),
                 call->Declaration()->source);
        return false;
    }

    return true;
}

bool Validator::FunctionCall(const sem::Call* call, sem::Statement* current_statement) const {
    auto* decl = call->Declaration();
    auto* target = call->Target()->As<sem::Function>();
    auto sym = decl->target.name->symbol;
    auto name = symbols_.NameFor(sym);

    if (!current_statement) {  // Function call at module-scope.
        AddError("functions cannot be called at module-scope", decl->source);
        return false;
    }

    if (target->Declaration()->IsEntryPoint()) {
        // https://www.w3.org/TR/WGSL/#function-restriction
        // An entry point must never be the target of a function call.
        AddError("entry point functions cannot be the target of a function call", decl->source);
        return false;
    }

    if (decl->args.Length() != target->Parameters().Length()) {
        bool more = decl->args.Length() > target->Parameters().Length();
        AddError("too " + (more ? std::string("many") : std::string("few")) +
                     " arguments in call to '" + name + "', expected " +
                     std::to_string(target->Parameters().Length()) + ", got " +
                     std::to_string(call->Arguments().Length()),
                 decl->source);
        return false;
    }

    for (size_t i = 0; i < call->Arguments().Length(); ++i) {
        const sem::Variable* param = target->Parameters()[i];
        const ast::Expression* arg_expr = decl->args[i];
        auto* param_type = param->Type();
        auto* arg_type = sem_.TypeOf(arg_expr)->UnwrapRef();

        if (param_type != arg_type) {
            AddError("type mismatch for argument " + std::to_string(i + 1) + " in call to '" +
                         name + "', expected '" + sem_.TypeNameOf(param_type) + "', got '" +
                         sem_.TypeNameOf(arg_type) + "'",
                     arg_expr->source);
            return false;
        }

        if (param_type->Is<sem::Pointer>()) {
            auto is_valid = false;
            if (auto* ident_expr = arg_expr->As<ast::IdentifierExpression>()) {
                auto* var = sem_.ResolvedSymbol<sem::Variable>(ident_expr);
                if (!var) {
                    TINT_ICE(Resolver, diagnostics_) << "failed to resolve identifier";
                    return false;
                }
                if (var->Is<sem::Parameter>()) {
                    is_valid = true;
                }
            } else if (auto* unary = arg_expr->As<ast::UnaryOpExpression>()) {
                if (unary->op == ast::UnaryOp::kAddressOf) {
                    if (auto* ident_unary = unary->expr->As<ast::IdentifierExpression>()) {
                        auto* var = sem_.ResolvedSymbol<sem::Variable>(ident_unary);
                        if (!var) {
                            TINT_ICE(Resolver, diagnostics_) << "failed to resolve identifier";
                            return false;
                        }
                        is_valid = true;
                    }
                }
            }

            if (!is_valid &&
                IsValidationEnabled(param->Declaration()->attributes,
                                    ast::DisabledValidation::kIgnoreInvalidPointerArgument)) {
                AddError(
                    "expected an address-of expression of a variable identifier "
                    "expression or a function parameter",
                    arg_expr->source);
                return false;
            }
        }
    }

    if (call->Type()->Is<sem::Void>()) {
        bool is_call_statement = false;
        if (auto* call_stmt = As<ast::CallStatement>(call->Stmt()->Declaration())) {
            if (call_stmt->expr == call->Declaration()) {
                is_call_statement = true;
            }
        }
        if (!is_call_statement) {
            // https://gpuweb.github.io/gpuweb/wgsl/#function-call-expr
            // If the called function does not return a value, a function call
            // statement should be used instead.
            AddError("function '" + name + "' does not return a value", decl->source);
            return false;
        }
    }

    if (call->Behaviors().Contains(sem::Behavior::kDiscard)) {
        if (auto* continuing = ClosestContinuing(/*stop_at_loop*/ false, current_statement)) {
            AddError("cannot call a function that may discard inside a continuing block",
                     call->Declaration()->source);
            if (continuing != call->Stmt()->Declaration() &&
                continuing != call->Stmt()->Parent()->Declaration()) {
                AddNote("see continuing block here", continuing->source);
            }
            return false;
        }
    }

    return true;
}

bool Validator::StructureConstructor(const ast::CallExpression* ctor,
                                     const sem::Struct* struct_type) const {
    if (!struct_type->IsConstructible()) {
        AddError("struct constructor has non-constructible type", ctor->source);
        return false;
    }

    if (ctor->args.Length() > 0) {
        if (ctor->args.Length() != struct_type->Members().size()) {
            std::string fm = ctor->args.Length() < struct_type->Members().size() ? "few" : "many";
            AddError("struct constructor has too " + fm + " inputs: expected " +
                         std::to_string(struct_type->Members().size()) + ", found " +
                         std::to_string(ctor->args.Length()),
                     ctor->source);
            return false;
        }
        for (auto* member : struct_type->Members()) {
            auto* value = ctor->args[member->Index()];
            auto* value_ty = sem_.TypeOf(value);
            if (member->Type() != value_ty->UnwrapRef()) {
                AddError(
                    "type in struct constructor does not match struct member type: "
                    "expected '" +
                        sem_.TypeNameOf(member->Type()) + "', found '" + sem_.TypeNameOf(value_ty) +
                        "'",
                    value->source);
                return false;
            }
        }
    }
    return true;
}

bool Validator::ArrayConstructor(const ast::CallExpression* ctor,
                                 const sem::Array* array_type) const {
    auto& values = ctor->args;
    auto* elem_ty = array_type->ElemType();
    for (auto* value : values) {
        auto* value_ty = sem_.TypeOf(value)->UnwrapRef();
        if (sem::Type::ConversionRank(value_ty, elem_ty) == sem::Type::kNoConversion) {
            AddError("'" + sem_.TypeNameOf(value_ty) +
                         "' cannot be used to construct an array of '" + sem_.TypeNameOf(elem_ty) +
                         "'",
                     value->source);
            return false;
        }
    }

    if (array_type->IsRuntimeSized()) {
        AddError("cannot construct a runtime-sized array", ctor->source);
        return false;
    } else if (!elem_ty->IsConstructible()) {
        AddError("array constructor has non-constructible element type", ctor->source);
        return false;
    } else if (!values.IsEmpty() && (values.Length() != array_type->Count())) {
        std::string fm = values.Length() < array_type->Count() ? "few" : "many";
        AddError("array constructor has too " + fm + " elements: expected " +
                     std::to_string(array_type->Count()) + ", found " +
                     std::to_string(values.Length()),
                 ctor->source);
        return false;
    } else if (values.Length() > array_type->Count()) {
        AddError("array constructor has too many elements: expected " +
                     std::to_string(array_type->Count()) + ", found " +
                     std::to_string(values.Length()),
                 ctor->source);
        return false;
    }
    return true;
}

bool Validator::Vector(const sem::Vector* ty, const Source& source) const {
    if (!ty->type()->is_scalar()) {
        AddError("vector element type must be 'bool', 'f32', 'f16', 'i32' or 'u32'", source);
        return false;
    }
    return true;
}

bool Validator::Matrix(const sem::Matrix* ty, const Source& source) const {
    if (!ty->is_float_matrix()) {
        AddError("matrix element type must be 'f32' or 'f16'", source);
        return false;
    }
    return true;
}

bool Validator::PipelineStages(const std::vector<sem::Function*>& entry_points) const {
    auto check_workgroup_storage = [&](const sem::Function* func,
                                       const sem::Function* entry_point) {
        auto stage = entry_point->Declaration()->PipelineStage();
        if (stage != ast::PipelineStage::kCompute) {
            for (auto* var : func->DirectlyReferencedGlobals()) {
                if (var->StorageClass() == ast::StorageClass::kWorkgroup) {
                    std::stringstream stage_name;
                    stage_name << stage;
                    for (auto* user : var->Users()) {
                        if (func == user->Stmt()->Function()) {
                            AddError("workgroup memory cannot be used by " + stage_name.str() +
                                         " pipeline stage",
                                     user->Declaration()->source);
                            break;
                        }
                    }
                    AddNote("variable is declared here", var->Declaration()->source);
                    if (func != entry_point) {
                        TraverseCallChain(
                            diagnostics_, entry_point, func, [&](const sem::Function* f) {
                                AddNote("called by function '" +
                                            symbols_.NameFor(f->Declaration()->symbol) + "'",
                                        f->Declaration()->source);
                            });
                        AddNote("called by entry point '" +
                                    symbols_.NameFor(entry_point->Declaration()->symbol) + "'",
                                entry_point->Declaration()->source);
                    }
                    return false;
                }
            }
        }
        return true;
    };

    for (auto* entry_point : entry_points) {
        if (!check_workgroup_storage(entry_point, entry_point)) {
            return false;
        }
        for (auto* func : entry_point->TransitivelyCalledFunctions()) {
            if (!check_workgroup_storage(func, entry_point)) {
                return false;
            }
        }
    }

    auto check_builtin_calls = [&](const sem::Function* func, const sem::Function* entry_point) {
        auto stage = entry_point->Declaration()->PipelineStage();
        for (auto* builtin : func->DirectlyCalledBuiltins()) {
            if (!builtin->SupportedStages().Contains(stage)) {
                auto* call = func->FindDirectCallTo(builtin);
                std::stringstream err;
                err << "built-in cannot be used by " << stage << " pipeline stage";
                AddError(err.str(),
                         call ? call->Declaration()->source : func->Declaration()->source);
                if (func != entry_point) {
                    TraverseCallChain(diagnostics_, entry_point, func, [&](const sem::Function* f) {
                        AddNote("called by function '" +
                                    symbols_.NameFor(f->Declaration()->symbol) + "'",
                                f->Declaration()->source);
                    });
                    AddNote("called by entry point '" +
                                symbols_.NameFor(entry_point->Declaration()->symbol) + "'",
                            entry_point->Declaration()->source);
                }
                return false;
            }
        }
        return true;
    };

    for (auto* entry_point : entry_points) {
        if (!check_builtin_calls(entry_point, entry_point)) {
            return false;
        }
        for (auto* func : entry_point->TransitivelyCalledFunctions()) {
            if (!check_builtin_calls(func, entry_point)) {
                return false;
            }
        }
    }
    return true;
}

bool Validator::PushConstants(const std::vector<sem::Function*>& entry_points) const {
    for (auto* entry_point : entry_points) {
        // State checked and modified by check_push_constant so that it remembers previously seen
        // push_constant variables for an entry-point.
        const sem::Variable* push_constant_var = nullptr;
        const sem::Function* push_constant_func = nullptr;

        auto check_push_constant = [&](const sem::Function* func, const sem::Function* ep) {
            for (auto* var : func->DirectlyReferencedGlobals()) {
                if (var->StorageClass() != ast::StorageClass::kPushConstant ||
                    var == push_constant_var) {
                    continue;
                }

                if (push_constant_var == nullptr) {
                    push_constant_var = var;
                    push_constant_func = func;
                    continue;
                }

                AddError("entry point '" + symbols_.NameFor(ep->Declaration()->symbol) +
                             "' uses two different 'push_constant' variables.",
                         ep->Declaration()->source);
                AddNote("first 'push_constant' variable declaration is here",
                        var->Declaration()->source);
                if (func != ep) {
                    TraverseCallChain(diagnostics_, ep, func, [&](const sem::Function* f) {
                        AddNote("called by function '" +
                                    symbols_.NameFor(f->Declaration()->symbol) + "'",
                                f->Declaration()->source);
                    });
                    AddNote("called by entry point '" +
                                symbols_.NameFor(ep->Declaration()->symbol) + "'",
                            ep->Declaration()->source);
                }
                AddNote("second 'push_constant' variable declaration is here",
                        push_constant_var->Declaration()->source);
                if (push_constant_func != ep) {
                    TraverseCallChain(
                        diagnostics_, ep, push_constant_func, [&](const sem::Function* f) {
                            AddNote("called by function '" +
                                        symbols_.NameFor(f->Declaration()->symbol) + "'",
                                    f->Declaration()->source);
                        });
                    AddNote("called by entry point '" +
                                symbols_.NameFor(ep->Declaration()->symbol) + "'",
                            ep->Declaration()->source);
                }
                return false;
            }

            return true;
        };

        if (!check_push_constant(entry_point, entry_point)) {
            return false;
        }
        for (auto* func : entry_point->TransitivelyCalledFunctions()) {
            if (!check_push_constant(func, entry_point)) {
                return false;
            }
        }
    }

    return true;
}

bool Validator::Array(const sem::Array* arr, const Source& source) const {
    auto* el_ty = arr->ElemType();

    if (!IsPlain(el_ty)) {
        AddError(sem_.TypeNameOf(el_ty) + " cannot be used as an element type of an array", source);
        return false;
    }

    if (!IsFixedFootprint(el_ty)) {
        AddError("an array element type cannot contain a runtime-sized array", source);
        return false;
    }
    return true;
}

bool Validator::ArrayStrideAttribute(const ast::StrideAttribute* attr,
                                     uint32_t el_size,
                                     uint32_t el_align) const {
    auto stride = attr->stride;
    bool is_valid_stride = (stride >= el_size) && (stride >= el_align) && (stride % el_align == 0);
    if (!is_valid_stride) {
        // https://gpuweb.github.io/gpuweb/wgsl/#array-layout-rules
        // Arrays decorated with the stride attribute must have a stride that is
        // at least the size of the element type, and be a multiple of the
        // element type's alignment value.
        AddError(
            "arrays decorated with the stride attribute must have a stride "
            "that is at least the size of the element type, and be a multiple "
            "of the element type's alignment value",
            attr->source);
        return false;
    }
    return true;
}

bool Validator::Alias(const ast::Alias* alias) const {
    auto name = symbols_.NameFor(alias->name);
    if (sem::ParseBuiltinType(name) != sem::BuiltinType::kNone) {
        AddError("'" + name + "' is a builtin and cannot be redeclared as an alias", alias->source);
        return false;
    }

    return true;
}

bool Validator::Structure(const sem::Struct* str, ast::PipelineStage stage) const {
    auto name = symbols_.NameFor(str->Declaration()->name);
    if (sem::ParseBuiltinType(name) != sem::BuiltinType::kNone) {
        AddError("'" + name + "' is a builtin and cannot be redeclared as a struct",
                 str->Declaration()->source);
        return false;
    }

    if (str->Members().empty()) {
        AddError("structures must have at least one member", str->Declaration()->source);
        return false;
    }

    std::unordered_set<uint32_t> locations;
    for (auto* member : str->Members()) {
        if (auto* r = member->Type()->As<sem::Array>()) {
            if (r->IsRuntimeSized()) {
                if (member != str->Members().back()) {
                    AddError("runtime arrays may only appear as the last member of a struct",
                             member->Declaration()->source);
                    return false;
                }
            }
        } else if (!IsFixedFootprint(member->Type())) {
            AddError(
                "a struct that contains a runtime array cannot be nested inside "
                "another struct",
                member->Declaration()->source);
            return false;
        }

        auto has_location = false;
        auto has_position = false;
        const ast::InvariantAttribute* invariant_attribute = nullptr;
        const ast::InterpolateAttribute* interpolate_attribute = nullptr;
        for (auto* attr : member->Declaration()->attributes) {
            if (!attr->IsAnyOf<ast::BuiltinAttribute,             //
                               ast::InternalAttribute,            //
                               ast::InterpolateAttribute,         //
                               ast::InvariantAttribute,           //
                               ast::LocationAttribute,            //
                               ast::StructMemberOffsetAttribute,  //
                               ast::StructMemberSizeAttribute,    //
                               ast::StructMemberAlignAttribute>()) {
                if (attr->Is<ast::StrideAttribute>() &&
                    IsValidationDisabled(member->Declaration()->attributes,
                                         ast::DisabledValidation::kIgnoreStrideAttribute)) {
                    continue;
                }
                AddError("attribute is not valid for structure members", attr->source);
                return false;
            }

            if (auto* invariant = attr->As<ast::InvariantAttribute>()) {
                invariant_attribute = invariant;
            } else if (auto* location = attr->As<ast::LocationAttribute>()) {
                has_location = true;
                if (!LocationAttribute(location, member->Type(), locations, stage,
                                       member->Declaration()->source)) {
                    return false;
                }
            } else if (auto* builtin = attr->As<ast::BuiltinAttribute>()) {
                if (!BuiltinAttribute(builtin, member->Type(), stage,
                                      /* is_input */ false)) {
                    return false;
                }
                if (builtin->builtin == ast::BuiltinValue::kPosition) {
                    has_position = true;
                }
            } else if (auto* interpolate = attr->As<ast::InterpolateAttribute>()) {
                interpolate_attribute = interpolate;
                if (!InterpolateAttribute(interpolate, member->Type())) {
                    return false;
                }
            }
        }

        if (invariant_attribute && !has_position) {
            AddError("invariant attribute must only be applied to a position builtin",
                     invariant_attribute->source);
            return false;
        }

        if (interpolate_attribute && !has_location) {
            AddError("interpolate attribute must only be used with @location",
                     interpolate_attribute->source);
            return false;
        }
    }

    for (auto* attr : str->Declaration()->attributes) {
        if (!(attr->IsAnyOf<ast::InternalAttribute>())) {
            AddError("attribute is not valid for struct declarations", attr->source);
            return false;
        }
    }

    return true;
}

bool Validator::LocationAttribute(const ast::LocationAttribute* location,
                                  const sem::Type* type,
                                  std::unordered_set<uint32_t>& locations,
                                  ast::PipelineStage stage,
                                  const Source& source,
                                  const bool is_input) const {
    std::string inputs_or_output = is_input ? "inputs" : "output";
    if (stage == ast::PipelineStage::kCompute) {
        AddError("attribute is not valid for compute shader " + inputs_or_output, location->source);
        return false;
    }

    if (!type->is_numeric_scalar_or_vector()) {
        std::string invalid_type = sem_.TypeNameOf(type);
        AddError("cannot apply 'location' attribute to declaration of type '" + invalid_type + "'",
                 source);
        AddNote(
            "'location' attribute must only be applied to declarations of "
            "numeric scalar or numeric vector type",
            location->source);
        return false;
    }

    if (locations.count(location->value)) {
        AddError(attr_to_str(location) + " attribute appears multiple times", location->source);
        return false;
    }
    locations.emplace(location->value);

    return true;
}

bool Validator::Return(const ast::ReturnStatement* ret,
                       const sem::Type* func_type,
                       const sem::Type* ret_type,
                       sem::Statement* current_statement) const {
    if (func_type->UnwrapRef() != ret_type) {
        AddError(
            "return statement type must match its function "
            "return type, returned '" +
                sem_.TypeNameOf(ret_type) + "', expected '" + sem_.TypeNameOf(func_type) + "'",
            ret->source);
        return false;
    }

    auto* sem = sem_.Get(ret);
    if (auto* continuing = ClosestContinuing(/*stop_at_loop*/ false, current_statement)) {
        AddError("continuing blocks must not contain a return statement", ret->source);
        if (continuing != sem->Declaration() && continuing != sem->Parent()->Declaration()) {
            AddNote("see continuing block here", continuing->source);
        }
        return false;
    }

    return true;
}

bool Validator::SwitchStatement(const ast::SwitchStatement* s) {
    auto* cond_ty = sem_.TypeOf(s->condition)->UnwrapRef();
    if (!cond_ty->is_integer_scalar()) {
        AddError(
            "switch statement selector expression must be of a "
            "scalar integer type",
            s->condition->source);
        return false;
    }

    bool has_default = false;
    std::unordered_map<int64_t, Source> selectors;

    for (auto* case_stmt : s->body) {
        if (case_stmt->IsDefault()) {
            if (has_default) {
                // More than one default clause
                AddError("switch statement must have exactly one default clause",
                         case_stmt->source);
                return false;
            }
            has_default = true;
        }

        for (auto* selector : case_stmt->selectors) {
            if (cond_ty != sem_.TypeOf(selector)) {
                AddError(
                    "the case selector values must have the same "
                    "type as the selector expression.",
                    case_stmt->source);
                return false;
            }

            auto it = selectors.find(selector->value);
            if (it != selectors.end()) {
                auto val = std::to_string(selector->value);
                AddError("duplicate switch case '" + val + "'", selector->source);
                AddNote("previous case declared here", it->second);
                return false;
            }
            selectors.emplace(selector->value, selector->source);
        }
    }

    if (!has_default) {
        // No default clause
        AddError("switch statement must have a default clause", s->source);
        return false;
    }

    return true;
}

bool Validator::Assignment(const ast::Statement* a, const sem::Type* rhs_ty) const {
    const ast::Expression* lhs;
    const ast::Expression* rhs;
    if (auto* assign = a->As<ast::AssignmentStatement>()) {
        lhs = assign->lhs;
        rhs = assign->rhs;
    } else if (auto* compound = a->As<ast::CompoundAssignmentStatement>()) {
        lhs = compound->lhs;
        rhs = compound->rhs;
    } else {
        TINT_ICE(Resolver, diagnostics_) << "invalid assignment statement";
        return false;
    }

    if (lhs->Is<ast::PhonyExpression>()) {
        // https://www.w3.org/TR/WGSL/#phony-assignment-section
        auto* ty = rhs_ty->UnwrapRef();
        if (!ty->IsConstructible() &&
            !ty->IsAnyOf<sem::Pointer, sem::Texture, sem::Sampler, sem::AbstractNumeric>()) {
            AddError("cannot assign '" + sem_.TypeNameOf(rhs_ty) +
                         "' to '_'. '_' can only be assigned a constructible, pointer, "
                         "texture or sampler type",
                     rhs->source);
            return false;
        }
        return true;  // RHS can be anything.
    }

    // https://gpuweb.github.io/gpuweb/wgsl/#assignment-statement
    auto const* lhs_ty = sem_.TypeOf(lhs);

    if (auto* variable = sem_.ResolvedSymbol<sem::Variable>(lhs)) {
        auto* v = variable->Declaration();
        const char* err = Switch(
            v,  //
            [&](const ast::Parameter*) { return "cannot assign to function parameter"; },
            [&](const ast::Let*) { return "cannot assign to 'let'"; },
            [&](const ast::Override*) { return "cannot assign to 'override'"; });
        if (err) {
            AddError(err, lhs->source);
            AddNote("'" + symbols_.NameFor(v->symbol) + "' is declared here:", v->source);
            return false;
        }
    }

    auto* lhs_ref = lhs_ty->As<sem::Reference>();
    if (!lhs_ref) {
        // LHS is not a reference, so it has no storage.
        AddError("cannot assign to value of type '" + sem_.TypeNameOf(lhs_ty) + "'", lhs->source);
        return false;
    }

    auto* storage_ty = lhs_ref->StoreType();
    auto* value_type = rhs_ty->UnwrapRef();  // Implicit load of RHS

    // Value type has to match storage type
    if (storage_ty != value_type) {
        AddError(
            "cannot assign '" + sem_.TypeNameOf(rhs_ty) + "' to '" + sem_.TypeNameOf(lhs_ty) + "'",
            a->source);
        return false;
    }
    if (!storage_ty->IsConstructible()) {
        AddError("storage type of assignment must be constructible", a->source);
        return false;
    }
    if (lhs_ref->Access() == ast::Access::kRead) {
        AddError("cannot store into a read-only type '" + sem_.RawTypeNameOf(lhs_ty) + "'",
                 a->source);
        return false;
    }
    return true;
}

bool Validator::IncrementDecrementStatement(const ast::IncrementDecrementStatement* inc) const {
    const ast::Expression* lhs = inc->lhs;

    // https://gpuweb.github.io/gpuweb/wgsl/#increment-decrement

    if (auto* variable = sem_.ResolvedSymbol<sem::Variable>(lhs)) {
        auto* v = variable->Declaration();
        const char* err = Switch(
            v,  //
            [&](const ast::Parameter*) { return "cannot modify function parameter"; },
            [&](const ast::Let*) { return "cannot modify 'let'"; },
            [&](const ast::Override*) { return "cannot modify 'override'"; });
        if (err) {
            AddError(err, lhs->source);
            AddNote("'" + symbols_.NameFor(v->symbol) + "' is declared here:", v->source);
            return false;
        }
    }

    auto const* lhs_ty = sem_.TypeOf(lhs);
    auto* lhs_ref = lhs_ty->As<sem::Reference>();
    if (!lhs_ref) {
        // LHS is not a reference, so it has no storage.
        AddError("cannot modify value of type '" + sem_.TypeNameOf(lhs_ty) + "'", lhs->source);
        return false;
    }

    if (!lhs_ref->StoreType()->is_integer_scalar()) {
        const std::string kind = inc->increment ? "increment" : "decrement";
        AddError(kind + " statement can only be applied to an integer scalar", lhs->source);
        return false;
    }

    if (lhs_ref->Access() == ast::Access::kRead) {
        AddError("cannot modify read-only type '" + sem_.RawTypeNameOf(lhs_ty) + "'", inc->source);
        return false;
    }
    return true;
}

bool Validator::NoDuplicateAttributes(utils::VectorRef<const ast::Attribute*> attributes) const {
    std::unordered_map<const TypeInfo*, Source> seen;
    for (auto* d : attributes) {
        auto res = seen.emplace(&d->TypeInfo(), d->source);
        if (!res.second && !d->Is<ast::InternalAttribute>()) {
            AddError("duplicate " + d->Name() + " attribute", d->source);
            AddNote("first attribute declared here", res.first->second);
            return false;
        }
    }
    return true;
}

bool Validator::IsValidationDisabled(utils::VectorRef<const ast::Attribute*> attributes,
                                     ast::DisabledValidation validation) const {
    for (auto* attribute : attributes) {
        if (auto* dv = attribute->As<ast::DisableValidationAttribute>()) {
            if (dv->validation == validation) {
                return true;
            }
        }
    }
    return false;
}

bool Validator::IsValidationEnabled(utils::VectorRef<const ast::Attribute*> attributes,
                                    ast::DisabledValidation validation) const {
    return !IsValidationDisabled(attributes, validation);
}

std::string Validator::VectorPretty(uint32_t size, const sem::Type* element_type) const {
    sem::Vector vec_type(element_type, size);
    return vec_type.FriendlyName(symbols_);
}

}  // namespace tint::resolver
