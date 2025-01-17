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

#include "gtest/gtest-spi.h"

#include "src/tint/ast/id_attribute.h"
#include "src/tint/ast/test_helper.h"

using namespace tint::number_suffixes;  // NOLINT

namespace tint::ast {
namespace {

using VariableTest = TestHelper;

TEST_F(VariableTest, Creation) {
    auto* v = Var("my_var", ty.i32(), StorageClass::kFunction);

    EXPECT_EQ(v->symbol, Symbol(1, ID()));
    EXPECT_EQ(v->declared_storage_class, StorageClass::kFunction);
    EXPECT_TRUE(v->type->Is<ast::I32>());
    EXPECT_EQ(v->source.range.begin.line, 0u);
    EXPECT_EQ(v->source.range.begin.column, 0u);
    EXPECT_EQ(v->source.range.end.line, 0u);
    EXPECT_EQ(v->source.range.end.column, 0u);
}

TEST_F(VariableTest, CreationWithSource) {
    auto* v = Var(Source{Source::Range{Source::Location{27, 4}, Source::Location{27, 5}}}, "i",
                  ty.f32(), StorageClass::kPrivate, utils::Empty);

    EXPECT_EQ(v->symbol, Symbol(1, ID()));
    EXPECT_EQ(v->declared_storage_class, StorageClass::kPrivate);
    EXPECT_TRUE(v->type->Is<ast::F32>());
    EXPECT_EQ(v->source.range.begin.line, 27u);
    EXPECT_EQ(v->source.range.begin.column, 4u);
    EXPECT_EQ(v->source.range.end.line, 27u);
    EXPECT_EQ(v->source.range.end.column, 5u);
}

TEST_F(VariableTest, CreationEmpty) {
    auto* v = Var(Source{Source::Range{Source::Location{27, 4}, Source::Location{27, 7}}}, "a_var",
                  ty.i32(), StorageClass::kWorkgroup, utils::Empty);

    EXPECT_EQ(v->symbol, Symbol(1, ID()));
    EXPECT_EQ(v->declared_storage_class, StorageClass::kWorkgroup);
    EXPECT_TRUE(v->type->Is<ast::I32>());
    EXPECT_EQ(v->source.range.begin.line, 27u);
    EXPECT_EQ(v->source.range.begin.column, 4u);
    EXPECT_EQ(v->source.range.end.line, 27u);
    EXPECT_EQ(v->source.range.end.column, 7u);
}

TEST_F(VariableTest, Assert_MissingSymbol) {
    EXPECT_FATAL_FAILURE(
        {
            ProgramBuilder b;
            b.Var("", b.ty.i32());
        },
        "internal compiler error");
}

TEST_F(VariableTest, Assert_DifferentProgramID_Symbol) {
    EXPECT_FATAL_FAILURE(
        {
            ProgramBuilder b1;
            ProgramBuilder b2;
            b1.Var(b2.Sym("x"), b1.ty.f32());
        },
        "internal compiler error");
}

TEST_F(VariableTest, Assert_DifferentProgramID_Constructor) {
    EXPECT_FATAL_FAILURE(
        {
            ProgramBuilder b1;
            ProgramBuilder b2;
            b1.Var("x", b1.ty.f32(), b2.Expr(1.2_f));
        },
        "internal compiler error");
}

TEST_F(VariableTest, WithAttributes) {
    auto* var = Var("my_var", ty.i32(), StorageClass::kFunction, Location(1u),
                    Builtin(BuiltinValue::kPosition), Id(1200u));

    auto& attributes = var->attributes;
    EXPECT_TRUE(ast::HasAttribute<ast::LocationAttribute>(attributes));
    EXPECT_TRUE(ast::HasAttribute<ast::BuiltinAttribute>(attributes));
    EXPECT_TRUE(ast::HasAttribute<ast::IdAttribute>(attributes));

    auto* location = ast::GetAttribute<ast::LocationAttribute>(attributes);
    ASSERT_NE(nullptr, location);
    EXPECT_EQ(1u, location->value);
}

TEST_F(VariableTest, HasBindingPoint_BothProvided) {
    auto* var = Var("my_var", ty.i32(), StorageClass::kFunction, Binding(2), Group(1));
    EXPECT_TRUE(var->HasBindingPoint());
}

TEST_F(VariableTest, HasBindingPoint_NeitherProvided) {
    auto* var = Var("my_var", ty.i32(), StorageClass::kFunction, utils::Empty);
    EXPECT_FALSE(var->HasBindingPoint());
}

TEST_F(VariableTest, HasBindingPoint_MissingGroupAttribute) {
    auto* var = Var("my_var", ty.i32(), StorageClass::kFunction, Binding(2));
    EXPECT_FALSE(var->HasBindingPoint());
}

TEST_F(VariableTest, HasBindingPoint_MissingBindingAttribute) {
    auto* var = Var("my_var", ty.i32(), StorageClass::kFunction, Group(1));
    EXPECT_FALSE(var->HasBindingPoint());
}

}  // namespace
}  // namespace tint::ast
