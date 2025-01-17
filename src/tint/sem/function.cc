// Copyright 2021 The Tint Authors.
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

#include "src/tint/sem/function.h"

#include "src/tint/ast/function.h"
#include "src/tint/sem/depth_texture.h"
#include "src/tint/sem/external_texture.h"
#include "src/tint/sem/multisampled_texture.h"
#include "src/tint/sem/sampled_texture.h"
#include "src/tint/sem/storage_texture.h"
#include "src/tint/sem/variable.h"
#include "src/tint/utils/transform.h"

TINT_INSTANTIATE_TYPEINFO(tint::sem::Function);

namespace tint::sem {
namespace {

utils::VectorRef<const Parameter*> SetOwner(utils::VectorRef<Parameter*> parameters,
                                            const tint::sem::CallTarget* owner) {
    for (auto* parameter : parameters) {
        parameter->SetOwner(owner);
    }
    return parameters;
}

}  // namespace

Function::Function(const ast::Function* declaration,
                   Type* return_type,
                   utils::VectorRef<Parameter*> parameters)
    : Base(return_type, SetOwner(std::move(parameters), this), EvaluationStage::kRuntime),
      declaration_(declaration),
      workgroup_size_{WorkgroupDimension{1}, WorkgroupDimension{1}, WorkgroupDimension{1}} {}

Function::~Function() = default;

std::vector<std::pair<const Variable*, const ast::LocationAttribute*>>
Function::TransitivelyReferencedLocationVariables() const {
    std::vector<std::pair<const Variable*, const ast::LocationAttribute*>> ret;

    for (auto* global : TransitivelyReferencedGlobals()) {
        for (auto* attr : global->Declaration()->attributes) {
            if (auto* location = attr->As<ast::LocationAttribute>()) {
                ret.push_back({global, location});
                break;
            }
        }
    }
    return ret;
}

Function::VariableBindings Function::TransitivelyReferencedUniformVariables() const {
    VariableBindings ret;

    for (auto* global : TransitivelyReferencedGlobals()) {
        if (global->StorageClass() != ast::StorageClass::kUniform) {
            continue;
        }

        if (global->Declaration()->HasBindingPoint()) {
            ret.push_back({global, global->BindingPoint()});
        }
    }
    return ret;
}

Function::VariableBindings Function::TransitivelyReferencedStorageBufferVariables() const {
    VariableBindings ret;

    for (auto* global : TransitivelyReferencedGlobals()) {
        if (global->StorageClass() != ast::StorageClass::kStorage) {
            continue;
        }

        if (global->Declaration()->HasBindingPoint()) {
            ret.push_back({global, global->BindingPoint()});
        }
    }
    return ret;
}

std::vector<std::pair<const Variable*, const ast::BuiltinAttribute*>>
Function::TransitivelyReferencedBuiltinVariables() const {
    std::vector<std::pair<const Variable*, const ast::BuiltinAttribute*>> ret;

    for (auto* global : TransitivelyReferencedGlobals()) {
        for (auto* attr : global->Declaration()->attributes) {
            if (auto* builtin = attr->As<ast::BuiltinAttribute>()) {
                ret.push_back({global, builtin});
                break;
            }
        }
    }
    return ret;
}

Function::VariableBindings Function::TransitivelyReferencedSamplerVariables() const {
    return TransitivelyReferencedSamplerVariablesImpl(ast::SamplerKind::kSampler);
}

Function::VariableBindings Function::TransitivelyReferencedComparisonSamplerVariables() const {
    return TransitivelyReferencedSamplerVariablesImpl(ast::SamplerKind::kComparisonSampler);
}

Function::VariableBindings Function::TransitivelyReferencedSampledTextureVariables() const {
    return TransitivelyReferencedSampledTextureVariablesImpl(false);
}

Function::VariableBindings Function::TransitivelyReferencedMultisampledTextureVariables() const {
    return TransitivelyReferencedSampledTextureVariablesImpl(true);
}

Function::VariableBindings Function::TransitivelyReferencedVariablesOfType(
    const tint::TypeInfo* type) const {
    VariableBindings ret;
    for (auto* global : TransitivelyReferencedGlobals()) {
        auto* unwrapped_type = global->Type()->UnwrapRef();
        if (unwrapped_type->TypeInfo().Is(type)) {
            if (global->Declaration()->HasBindingPoint()) {
                ret.push_back({global, global->BindingPoint()});
            }
        }
    }
    return ret;
}

bool Function::HasAncestorEntryPoint(Symbol symbol) const {
    for (const auto* point : ancestor_entry_points_) {
        if (point->Declaration()->symbol == symbol) {
            return true;
        }
    }
    return false;
}

Function::VariableBindings Function::TransitivelyReferencedSamplerVariablesImpl(
    ast::SamplerKind kind) const {
    VariableBindings ret;

    for (auto* global : TransitivelyReferencedGlobals()) {
        auto* unwrapped_type = global->Type()->UnwrapRef();
        auto* sampler = unwrapped_type->As<sem::Sampler>();
        if (sampler == nullptr || sampler->kind() != kind) {
            continue;
        }

        if (global->Declaration()->HasBindingPoint()) {
            ret.push_back({global, global->BindingPoint()});
        }
    }
    return ret;
}

Function::VariableBindings Function::TransitivelyReferencedSampledTextureVariablesImpl(
    bool multisampled) const {
    VariableBindings ret;

    for (auto* global : TransitivelyReferencedGlobals()) {
        auto* unwrapped_type = global->Type()->UnwrapRef();
        auto* texture = unwrapped_type->As<sem::Texture>();
        if (texture == nullptr) {
            continue;
        }

        auto is_multisampled = texture->Is<sem::MultisampledTexture>();
        auto is_sampled = texture->Is<sem::SampledTexture>();

        if ((multisampled && !is_multisampled) || (!multisampled && !is_sampled)) {
            continue;
        }

        if (global->Declaration()->HasBindingPoint()) {
            ret.push_back({global, global->BindingPoint()});
        }
    }

    return ret;
}

}  // namespace tint::sem
