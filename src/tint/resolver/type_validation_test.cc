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

#include "src/tint/ast/id_attribute.h"
#include "src/tint/ast/return_statement.h"
#include "src/tint/ast/stage_attribute.h"
#include "src/tint/resolver/resolver.h"
#include "src/tint/resolver/resolver_test_helper.h"
#include "src/tint/sem/multisampled_texture.h"
#include "src/tint/sem/storage_texture.h"

#include "gmock/gmock.h"

using namespace tint::number_suffixes;  // NOLINT

namespace tint::resolver {
namespace {

// Helpers and typedefs
template <typename T>
using DataType = builder::DataType<T>;
template <typename T>
using vec2 = builder::vec2<T>;
template <typename T>
using vec3 = builder::vec3<T>;
template <typename T>
using vec4 = builder::vec4<T>;
template <typename T>
using mat2x2 = builder::mat2x2<T>;
template <typename T>
using mat3x3 = builder::mat3x3<T>;
template <typename T>
using mat4x4 = builder::mat4x4<T>;
template <int N, typename T>
using array = builder::array<N, T>;
template <typename T>
using alias = builder::alias<T>;
template <typename T>
using alias1 = builder::alias1<T>;
template <typename T>
using alias2 = builder::alias2<T>;
template <typename T>
using alias3 = builder::alias3<T>;

class ResolverTypeValidationTest : public resolver::TestHelper, public testing::Test {};

TEST_F(ResolverTypeValidationTest, VariableDeclNoConstructor_Pass) {
    // {
    // var a :i32;
    // a = 2;
    // }
    auto* var = Var("a", ty.i32());
    auto* lhs = Expr("a");
    auto* rhs = Expr(2_i);

    auto* body = Block(Decl(var), Assign(Source{Source::Location{12, 34}}, lhs, rhs));

    WrapInFunction(body);

    EXPECT_TRUE(r()->Resolve()) << r()->error();
    ASSERT_NE(TypeOf(lhs), nullptr);
    ASSERT_NE(TypeOf(rhs), nullptr);
}

TEST_F(ResolverTypeValidationTest, GlobalOverrideNoConstructor_Pass) {
    // @id(0) override a :i32;
    Override(Source{{12, 34}}, "a", ty.i32(), Id(0));

    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, GlobalVariableWithStorageClass_Pass) {
    // var<private> global_var: f32;
    GlobalVar(Source{{12, 34}}, "global_var", ty.f32(), ast::StorageClass::kPrivate);

    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, GlobalConstNoStorageClass_Pass) {
    // const global_const: f32 = f32();
    GlobalConst(Source{{12, 34}}, "global_const", ty.f32(), Construct(ty.f32()));

    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, GlobalVariableUnique_Pass) {
    // var global_var0 : f32 = 0.1;
    // var global_var1 : i32 = 0;

    GlobalVar("global_var0", ty.f32(), ast::StorageClass::kPrivate, Expr(0.1_f));

    GlobalVar(Source{{12, 34}}, "global_var1", ty.f32(), ast::StorageClass::kPrivate, Expr(1_f));

    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, GlobalVariableFunctionVariableNotUnique_Pass) {
    // fn my_func() {
    //   var a: f32 = 2.0;
    // }
    // var a: f32 = 2.1;

    Func("my_func", utils::Empty, ty.void_(),
         utils::Vector{
             Decl(Var("a", ty.f32(), Expr(2_f))),
         });

    GlobalVar("a", ty.f32(), ast::StorageClass::kPrivate, Expr(2.1_f));

    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, RedeclaredIdentifierInnerScope_Pass) {
    // {
    // if (true) { var a : f32 = 2.0; }
    // var a : f32 = 3.14;
    // }
    auto* var = Var("a", ty.f32(), Expr(2_f));

    auto* cond = Expr(true);
    auto* body = Block(Decl(var));

    auto* var_a_float = Var("a", ty.f32(), Expr(3.1_f));

    auto* outer_body = Block(If(cond, body), Decl(Source{{12, 34}}, var_a_float));

    WrapInFunction(outer_body);

    EXPECT_TRUE(r()->Resolve());
}

TEST_F(ResolverTypeValidationTest, RedeclaredIdentifierInnerScopeBlock_Pass) {
    // {
    //  { var a : f32; }
    //  var a : f32;
    // }
    auto* var_inner = Var("a", ty.f32());
    auto* inner = Block(Decl(Source{{12, 34}}, var_inner));

    auto* var_outer = Var("a", ty.f32());
    auto* outer_body = Block(inner, Decl(var_outer));

    WrapInFunction(outer_body);

    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, RedeclaredIdentifierDifferentFunctions_Pass) {
    // func0 { var a : f32 = 2.0; return; }
    // func1 { var a : f32 = 3.0; return; }
    auto* var0 = Var("a", ty.f32(), Expr(2_f));

    auto* var1 = Var("a", ty.f32(), Expr(1_f));

    Func("func0", utils::Empty, ty.void_(),
         utils::Vector{
             Decl(Source{{12, 34}}, var0),
             Return(),
         });

    Func("func1", utils::Empty, ty.void_(),
         utils::Vector{
             Decl(Source{{13, 34}}, var1),
             Return(),
         });

    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, ArraySize_AIntLiteral_Pass) {
    // var<private> a : array<f32, 4>;
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, 4_a)), ast::StorageClass::kPrivate);
    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, ArraySize_UnsignedLiteral_Pass) {
    // var<private> a : array<f32, 4u>;
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, 4_u)), ast::StorageClass::kPrivate);
    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, ArraySize_SignedLiteral_Pass) {
    // var<private> a : array<f32, 4i>;
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, 4_i)), ast::StorageClass::kPrivate);
    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, ArraySize_UnsignedConst_Pass) {
    // const size = 4u;
    // var<private> a : array<f32, size>;
    GlobalConst("size", Expr(4_u));
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, "size")), ast::StorageClass::kPrivate);
    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, ArraySize_SignedConst_Pass) {
    // const size = 4i;
    // var<private> a : array<f32, size>;
    GlobalConst("size", Expr(4_i));
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, "size")), ast::StorageClass::kPrivate);
    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, ArraySize_AIntLiteral_Zero) {
    // var<private> a : array<f32, 0>;
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, 0_a)), ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(), "12:34 error: array size (0) must be greater than 0");
}

TEST_F(ResolverTypeValidationTest, ArraySize_UnsignedLiteral_Zero) {
    // var<private> a : array<f32, 0u>;
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, 0_u)), ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(), "12:34 error: array size (0) must be greater than 0");
}

TEST_F(ResolverTypeValidationTest, ArraySize_SignedLiteral_Zero) {
    // var<private> a : array<f32, 0i>;
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, 0_i)), ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(), "12:34 error: array size (0) must be greater than 0");
}

TEST_F(ResolverTypeValidationTest, ArraySize_SignedLiteral_Negative) {
    // var<private> a : array<f32, -10i>;
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, -10_i)), ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(), "12:34 error: array size (-10) must be greater than 0");
}

TEST_F(ResolverTypeValidationTest, ArraySize_UnsignedConst_Zero) {
    // const size = 0u;
    // var<private> a : array<f32, size>;
    GlobalConst("size", Expr(0_u));
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, "size")), ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(), "12:34 error: array size (0) must be greater than 0");
}

TEST_F(ResolverTypeValidationTest, ArraySize_SignedConst_Zero) {
    // const size = 0i;
    // var<private> a : array<f32, size>;
    GlobalConst("size", Expr(0_i));
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, "size")), ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(), "12:34 error: array size (0) must be greater than 0");
}

TEST_F(ResolverTypeValidationTest, ArraySize_SignedConst_Negative) {
    // const size = -10i;
    // var<private> a : array<f32, size>;
    GlobalConst("size", Expr(-10_i));
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, "size")), ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(), "12:34 error: array size (-10) must be greater than 0");
}

TEST_F(ResolverTypeValidationTest, ArraySize_FloatLiteral) {
    // var<private> a : array<f32, 10.0>;
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, 10_f)), ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              "12:34 error: array size must evaluate to a constant integer expression, but is type "
              "'f32'");
}

TEST_F(ResolverTypeValidationTest, ArraySize_IVecLiteral) {
    // var<private> a : array<f32, vec2<i32>(10, 10)>;
    GlobalVar("a", ty.array(ty.f32(), Construct(Source{{12, 34}}, ty.vec2<i32>(), 10_i, 10_i)),
              ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              "12:34 error: array size must evaluate to a constant integer expression, but is type "
              "'vec2<i32>'");
}

TEST_F(ResolverTypeValidationTest, ArraySize_FloatConst) {
    // const size = 10.0;
    // var<private> a : array<f32, size>;
    GlobalConst("size", Expr(10_f));
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, "size")), ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              "12:34 error: array size must evaluate to a constant integer expression, but is type "
              "'f32'");
}

TEST_F(ResolverTypeValidationTest, ArraySize_IVecConst) {
    // const size = vec2<i32>(100, 100);
    // var<private> a : array<f32, size>;
    GlobalConst("size", Construct(ty.vec2<i32>(), 100_i, 100_i));
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, "size")), ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              "12:34 error: array size must evaluate to a constant integer expression, but is type "
              "'vec2<i32>'");
}

TEST_F(ResolverTypeValidationTest, ArraySize_TooBig_ImplicitStride) {
    // var<private> a : array<f32, 0x40000000u>;
    GlobalVar("a", ty.array(Source{{12, 34}}, ty.f32(), 0x40000000_u), ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              "12:34 error: array size (0x100000000) must not exceed 0xffffffff bytes");
}

TEST_F(ResolverTypeValidationTest, ArraySize_TooBig_ExplicitStride) {
    // var<private> a : @stride(8) array<f32, 0x20000000u>;
    GlobalVar("a", ty.array(Source{{12, 34}}, ty.f32(), 0x20000000_u, 8),
              ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              "12:34 error: array size (0x100000000) must not exceed 0xffffffff bytes");
}

TEST_F(ResolverTypeValidationTest, ArraySize_Overridable) {
    // override size = 10i;
    // var<private> a : array<f32, size>;
    Override("size", Expr(10_i));
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, "size")), ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              "12:34 error: array size must evaluate to a constant integer expression");
}

TEST_F(ResolverTypeValidationTest, ArraySize_ModuleVar) {
    // var<private> size : i32 = 10i;
    // var<private> a : array<f32, size>;
    GlobalVar("size", ty.i32(), Expr(10_i), ast::StorageClass::kPrivate);
    GlobalVar("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, "size")), ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              R"(12:34 error: var 'size' cannot not be referenced at module-scope
note: var 'size' declared here)");
}

TEST_F(ResolverTypeValidationTest, ArraySize_FunctionConst) {
    // {
    //   const size = 10;
    //   var a : array<f32, size>;
    // }
    auto* size = Const("size", Expr(10_i));
    auto* a = Var("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, "size")));
    WrapInFunction(size, a);
    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, ArraySize_FunctionLet) {
    // {
    //   let size = 10;
    //   var a : array<f32, size>;
    // }
    auto* size = Let("size", Expr(10_i));
    auto* a = Var("a", ty.array(ty.f32(), Expr(Source{{12, 34}}, "size")));
    WrapInFunction(size, a);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              "12:34 error: array size must evaluate to a constant integer expression");
}

TEST_F(ResolverTypeValidationTest, ArraySize_ComplexExpr) {
    // var a : array<f32, i32(4i)>;
    auto* a = Var("a", ty.array(ty.f32(), Construct(Source{{12, 34}}, ty.i32(), 4_i)));
    WrapInFunction(a);
    EXPECT_TRUE(r()->Resolve());
}

TEST_F(ResolverTypeValidationTest, RuntimeArrayInFunction_Fail) {
    /// @vertex
    // fn func() { var a : array<i32>; }

    auto* var = Var(Source{{12, 34}}, "a", ty.array<i32>());

    Func("func", utils::Empty, ty.void_(),
         utils::Vector{
             Decl(var),
         },
         utils::Vector{
             Stage(ast::PipelineStage::kVertex),
         });

    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              R"(12:34 error: runtime-sized arrays can only be used in the <storage> storage class
12:34 note: while instantiating 'var' a)");
}

TEST_F(ResolverTypeValidationTest, Struct_Member_VectorNoType) {
    // struct S {
    //   a: vec3;
    // };

    Structure("S", utils::Vector{
                       Member("a", create<ast::Vector>(Source{{12, 34}}, nullptr, 3u)),
                   });

    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(), "12:34 error: missing vector element type");
}

TEST_F(ResolverTypeValidationTest, Struct_Member_MatrixNoType) {
    // struct S {
    //   a: mat3x3;
    // };
    Structure("S", utils::Vector{
                       Member("a", create<ast::Matrix>(Source{{12, 34}}, nullptr, 3u, 3u)),
                   });

    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(), "12:34 error: missing matrix element type");
}

TEST_F(ResolverTypeValidationTest, Struct_TooBig) {
    // struct Foo {
    //   a: array<f32, 0x20000000>;
    //   b: array<f32, 0x20000000>;
    // };

    Structure(Source{{12, 34}}, "Foo",
              utils::Vector{
                  Member("a", ty.array<f32, 0x20000000>()),
                  Member("b", ty.array<f32, 0x20000000>()),
              });

    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              "12:34 error: struct size (0x100000000) must not exceed 0xffffffff bytes");
}

TEST_F(ResolverTypeValidationTest, Struct_MemberOffset_TooBig) {
    // struct Foo {
    //   a: array<f32, 0x3fffffff>;
    //   b: f32;
    //   c: f32;
    // };

    Structure("Foo", utils::Vector{
                         Member("a", ty.array<f32, 0x3fffffff>()),
                         Member("b", ty.f32()),
                         Member(Source{{12, 34}}, "c", ty.f32()),
                     });

    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              "12:34 error: struct member offset (0x100000000) must not exceed 0xffffffff bytes");
}

TEST_F(ResolverTypeValidationTest, RuntimeArrayIsLast_Pass) {
    // struct Foo {
    //   vf: f32;
    //   rt: array<f32>;
    // };

    Structure("Foo", utils::Vector{
                         Member("vf", ty.f32()),
                         Member("rt", ty.array<f32>()),
                     });

    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, RuntimeArrayInArray) {
    // struct Foo {
    //   rt : array<array<f32>, 4u>;
    // };

    Structure("Foo", utils::Vector{
                         Member("rt", ty.array(Source{{12, 34}}, ty.array<f32>(), 4_u)),
                     });

    EXPECT_FALSE(r()->Resolve()) << r()->error();
    EXPECT_EQ(r()->error(),
              "12:34 error: an array element type cannot contain a runtime-sized array");
}

TEST_F(ResolverTypeValidationTest, RuntimeArrayInStructInArray) {
    // struct Foo {
    //   rt : array<f32>;
    // };
    // var<private> a : array<Foo, 4>;

    auto* foo = Structure("Foo", utils::Vector{
                                     Member("rt", ty.array<f32>()),
                                 });
    GlobalVar("v", ty.array(Source{{12, 34}}, ty.Of(foo), 4_u), ast::StorageClass::kPrivate);

    EXPECT_FALSE(r()->Resolve()) << r()->error();
    EXPECT_EQ(r()->error(),
              "12:34 error: an array element type cannot contain a runtime-sized array");
}

TEST_F(ResolverTypeValidationTest, RuntimeArrayInStructInStruct) {
    // struct Foo {
    //   rt : array<f32>;
    // };
    // struct Outer {
    //   inner : Foo;
    // };

    auto* foo = Structure("Foo", utils::Vector{
                                     Member("rt", ty.array<f32>()),
                                 });
    Structure("Outer", utils::Vector{
                           Member(Source{{12, 34}}, "inner", ty.Of(foo)),
                       });

    EXPECT_FALSE(r()->Resolve()) << r()->error();
    EXPECT_EQ(r()->error(),
              "12:34 error: a struct that contains a runtime array cannot be nested inside another "
              "struct");
}

TEST_F(ResolverTypeValidationTest, RuntimeArrayIsNotLast_Fail) {
    // struct Foo {
    //   rt: array<f32>;
    //   vf: f32;
    // };

    Structure("Foo", utils::Vector{
                         Member(Source{{12, 34}}, "rt", ty.array<f32>()),
                         Member("vf", ty.f32()),
                     });

    EXPECT_FALSE(r()->Resolve()) << r()->error();
    EXPECT_EQ(r()->error(),
              R"(12:34 error: runtime arrays may only appear as the last member of a struct)");
}

TEST_F(ResolverTypeValidationTest, RuntimeArrayAsGlobalVariable) {
    GlobalVar(Source{{56, 78}}, "g", ty.array<i32>(), ast::StorageClass::kPrivate);

    ASSERT_FALSE(r()->Resolve());

    EXPECT_EQ(r()->error(),
              R"(56:78 error: runtime-sized arrays can only be used in the <storage> storage class
56:78 note: while instantiating 'var' g)");
}

TEST_F(ResolverTypeValidationTest, RuntimeArrayAsLocalVariable) {
    auto* v = Var(Source{{56, 78}}, "g", ty.array<i32>());
    WrapInFunction(v);

    ASSERT_FALSE(r()->Resolve());

    EXPECT_EQ(r()->error(),
              R"(56:78 error: runtime-sized arrays can only be used in the <storage> storage class
56:78 note: while instantiating 'var' g)");
}

TEST_F(ResolverTypeValidationTest, RuntimeArrayAsParameter_Fail) {
    // fn func(a : array<u32>) {}
    // @vertex fn main() {}

    auto* param = Param(Source{{12, 34}}, "a", ty.array<i32>());

    Func("func", utils::Vector{param}, ty.void_(),
         utils::Vector{
             Return(),
         });

    Func("main", utils::Empty, ty.void_(),
         utils::Vector{
             Return(),
         },
         utils::Vector{
             Stage(ast::PipelineStage::kVertex),
         });

    EXPECT_FALSE(r()->Resolve()) << r()->error();
    EXPECT_EQ(r()->error(),
              R"(12:34 error: runtime-sized arrays can only be used in the <storage> storage class
12:34 note: while instantiating parameter a)");
}

TEST_F(ResolverTypeValidationTest, PtrToRuntimeArrayAsParameter_Fail) {
    // fn func(a : ptr<workgroup, array<u32>>) {}

    auto* param =
        Param(Source{{12, 34}}, "a", ty.pointer(ty.array<i32>(), ast::StorageClass::kWorkgroup));

    Func("func", utils::Vector{param}, ty.void_(),
         utils::Vector{
             Return(),
         });

    EXPECT_FALSE(r()->Resolve()) << r()->error();
    EXPECT_EQ(r()->error(),
              R"(12:34 error: runtime-sized arrays can only be used in the <storage> storage class
12:34 note: while instantiating parameter a)");
}

TEST_F(ResolverTypeValidationTest, AliasRuntimeArrayIsNotLast_Fail) {
    // type RTArr = array<u32>;
    // struct s {
    //  b: RTArr;
    //  a: u32;
    //}

    auto* alias = Alias("RTArr", ty.array<u32>());
    Structure("s", utils::Vector{
                       Member(Source{{12, 34}}, "b", ty.Of(alias)),
                       Member("a", ty.u32()),
                   });

    EXPECT_FALSE(r()->Resolve()) << r()->error();
    EXPECT_EQ(r()->error(),
              "12:34 error: runtime arrays may only appear as the last member of a struct");
}

TEST_F(ResolverTypeValidationTest, AliasRuntimeArrayIsLast_Pass) {
    // type RTArr = array<u32>;
    // struct s {
    //  a: u32;
    //  b: RTArr;
    //}

    auto* alias = Alias("RTArr", ty.array<u32>());
    Structure("s", utils::Vector{
                       Member("a", ty.u32()),
                       Member("b", ty.Of(alias)),
                   });

    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, ArrayOfNonStorableType) {
    auto* tex_ty = ty.sampled_texture(ast::TextureDimension::k2d, ty.f32());
    GlobalVar("arr", ty.array(Source{{12, 34}}, tex_ty, 4_i), ast::StorageClass::kPrivate);

    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              "12:34 error: texture_2d<f32> cannot be used as an element type of an array");
}

TEST_F(ResolverTypeValidationTest, VariableAsType) {
    // var<private> a : i32;
    // var<private> b : a;
    GlobalVar("a", ty.i32(), ast::StorageClass::kPrivate);
    GlobalVar("b", ty.type_name("a"), ast::StorageClass::kPrivate);

    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              R"(error: cannot use variable 'a' as type
note: 'a' declared here)");
}

TEST_F(ResolverTypeValidationTest, FunctionAsType) {
    // fn f() {}
    // var<private> v : f;
    Func("f", utils::Empty, ty.void_(), {});
    GlobalVar("v", ty.type_name("f"), ast::StorageClass::kPrivate);

    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              R"(error: cannot use function 'f' as type
note: 'f' declared here)");
}

TEST_F(ResolverTypeValidationTest, BuiltinAsType) {
    // var<private> v : max;
    GlobalVar("v", ty.type_name("max"), ast::StorageClass::kPrivate);

    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(), "error: cannot use builtin 'max' as type");
}

TEST_F(ResolverTypeValidationTest, F16TypeUsedWithExtension) {
    // enable f16;
    // var<private> v : f16;
    Enable(ast::Extension::kF16);

    GlobalVar("v", ty.f16(), ast::StorageClass::kPrivate);

    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

TEST_F(ResolverTypeValidationTest, F16TypeUsedWithoutExtension) {
    // var<private> v : f16;
    GlobalVar("v", ty.f16(), ast::StorageClass::kPrivate);

    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(), "error: f16 used without 'f16' extension enabled");
}

namespace GetCanonicalTests {
struct Params {
    builder::ast_type_func_ptr create_ast_type;
    builder::sem_type_func_ptr create_sem_type;
};

template <typename T>
constexpr Params ParamsFor() {
    return Params{DataType<T>::AST, DataType<T>::Sem};
}

static constexpr Params cases[] = {
    ParamsFor<bool>(),
    ParamsFor<alias<bool>>(),
    ParamsFor<alias1<alias<bool>>>(),

    ParamsFor<vec3<f32>>(),
    ParamsFor<alias<vec3<f32>>>(),
    ParamsFor<alias1<alias<vec3<f32>>>>(),

    ParamsFor<vec3<alias<f32>>>(),
    ParamsFor<alias1<vec3<alias<f32>>>>(),
    ParamsFor<alias2<alias1<vec3<alias<f32>>>>>(),
    ParamsFor<alias3<alias2<vec3<alias1<alias<f32>>>>>>(),

    ParamsFor<mat3x3<alias<f32>>>(),
    ParamsFor<alias1<mat3x3<alias<f32>>>>(),
    ParamsFor<alias2<alias1<mat3x3<alias<f32>>>>>(),
    ParamsFor<alias3<alias2<mat3x3<alias1<alias<f32>>>>>>(),

    ParamsFor<alias1<alias<bool>>>(),
    ParamsFor<alias1<alias<vec3<f32>>>>(),
    ParamsFor<alias1<alias<mat3x3<f32>>>>(),
};

using CanonicalTest = ResolverTestWithParam<Params>;
TEST_P(CanonicalTest, All) {
    auto& params = GetParam();

    auto* type = params.create_ast_type(*this);

    auto* var = Var("v", type);
    auto* expr = Expr("v");
    WrapInFunction(var, expr);

    EXPECT_TRUE(r()->Resolve()) << r()->error();

    auto* got = TypeOf(expr)->UnwrapRef();
    auto* expected = params.create_sem_type(*this);

    EXPECT_EQ(got, expected) << "got:      " << FriendlyName(got) << "\n"
                             << "expected: " << FriendlyName(expected) << "\n";
}
INSTANTIATE_TEST_SUITE_P(ResolverTypeValidationTest, CanonicalTest, testing::ValuesIn(cases));

}  // namespace GetCanonicalTests

namespace SampledTextureTests {
struct DimensionParams {
    ast::TextureDimension dim;
    bool is_valid;
};

using SampledTextureDimensionTest = ResolverTestWithParam<DimensionParams>;
TEST_P(SampledTextureDimensionTest, All) {
    auto& params = GetParam();
    GlobalVar(Source{{12, 34}}, "a", ty.sampled_texture(params.dim, ty.i32()), Group(0),
              Binding(0));

    EXPECT_TRUE(r()->Resolve()) << r()->error();
}
INSTANTIATE_TEST_SUITE_P(ResolverTypeValidationTest,
                         SampledTextureDimensionTest,
                         testing::Values(  //
                             DimensionParams{ast::TextureDimension::k1d, true},
                             DimensionParams{ast::TextureDimension::k2d, true},
                             DimensionParams{ast::TextureDimension::k2dArray, true},
                             DimensionParams{ast::TextureDimension::k3d, true},
                             DimensionParams{ast::TextureDimension::kCube, true},
                             DimensionParams{ast::TextureDimension::kCubeArray, true}));

using MultisampledTextureDimensionTest = ResolverTestWithParam<DimensionParams>;
TEST_P(MultisampledTextureDimensionTest, All) {
    auto& params = GetParam();
    GlobalVar("a", ty.multisampled_texture(Source{{12, 34}}, params.dim, ty.i32()), Group(0),
              Binding(0));

    if (params.is_valid) {
        EXPECT_TRUE(r()->Resolve()) << r()->error();
    } else {
        EXPECT_FALSE(r()->Resolve());
        EXPECT_EQ(r()->error(), "12:34 error: only 2d multisampled textures are supported");
    }
}
INSTANTIATE_TEST_SUITE_P(ResolverTypeValidationTest,
                         MultisampledTextureDimensionTest,
                         testing::Values(  //
                             DimensionParams{ast::TextureDimension::k1d, false},
                             DimensionParams{ast::TextureDimension::k2d, true},
                             DimensionParams{ast::TextureDimension::k2dArray, false},
                             DimensionParams{ast::TextureDimension::k3d, false},
                             DimensionParams{ast::TextureDimension::kCube, false},
                             DimensionParams{ast::TextureDimension::kCubeArray, false}));

struct TypeParams {
    builder::ast_type_func_ptr type_func;
    bool is_valid;
};

template <typename T>
constexpr TypeParams TypeParamsFor(bool is_valid) {
    return TypeParams{DataType<T>::AST, is_valid};
}

static constexpr TypeParams type_cases[] = {
    TypeParamsFor<bool>(false),
    TypeParamsFor<i32>(true),
    TypeParamsFor<u32>(true),
    TypeParamsFor<f32>(true),

    TypeParamsFor<alias<bool>>(false),
    TypeParamsFor<alias<i32>>(true),
    TypeParamsFor<alias<u32>>(true),
    TypeParamsFor<alias<f32>>(true),

    TypeParamsFor<vec3<f32>>(false),
    TypeParamsFor<mat3x3<f32>>(false),

    TypeParamsFor<alias<vec3<f32>>>(false),
    TypeParamsFor<alias<mat3x3<f32>>>(false),
};

using SampledTextureTypeTest = ResolverTestWithParam<TypeParams>;
TEST_P(SampledTextureTypeTest, All) {
    auto& params = GetParam();
    GlobalVar(
        "a",
        ty.sampled_texture(Source{{12, 34}}, ast::TextureDimension::k2d, params.type_func(*this)),
        Group(0), Binding(0));

    if (params.is_valid) {
        EXPECT_TRUE(r()->Resolve()) << r()->error();
    } else {
        EXPECT_FALSE(r()->Resolve());
        EXPECT_EQ(r()->error(), "12:34 error: texture_2d<type>: type must be f32, i32 or u32");
    }
}
INSTANTIATE_TEST_SUITE_P(ResolverTypeValidationTest,
                         SampledTextureTypeTest,
                         testing::ValuesIn(type_cases));

using MultisampledTextureTypeTest = ResolverTestWithParam<TypeParams>;
TEST_P(MultisampledTextureTypeTest, All) {
    auto& params = GetParam();
    GlobalVar("a",
              ty.multisampled_texture(Source{{12, 34}}, ast::TextureDimension::k2d,
                                      params.type_func(*this)),
              Group(0), Binding(0));

    if (params.is_valid) {
        EXPECT_TRUE(r()->Resolve()) << r()->error();
    } else {
        EXPECT_FALSE(r()->Resolve());
        EXPECT_EQ(r()->error(),
                  "12:34 error: texture_multisampled_2d<type>: type must be f32, i32 or u32");
    }
}
INSTANTIATE_TEST_SUITE_P(ResolverTypeValidationTest,
                         MultisampledTextureTypeTest,
                         testing::ValuesIn(type_cases));

}  // namespace SampledTextureTests

namespace StorageTextureTests {
struct DimensionParams {
    ast::TextureDimension dim;
    bool is_valid;
};

static constexpr DimensionParams Dimension_cases[] = {
    DimensionParams{ast::TextureDimension::k1d, true},
    DimensionParams{ast::TextureDimension::k2d, true},
    DimensionParams{ast::TextureDimension::k2dArray, true},
    DimensionParams{ast::TextureDimension::k3d, true},
    DimensionParams{ast::TextureDimension::kCube, false},
    DimensionParams{ast::TextureDimension::kCubeArray, false}};

using StorageTextureDimensionTest = ResolverTestWithParam<DimensionParams>;
TEST_P(StorageTextureDimensionTest, All) {
    // @group(0) @binding(0)
    // var a : texture_storage_*<ru32int, write>;
    auto& params = GetParam();

    auto* st = ty.storage_texture(Source{{12, 34}}, params.dim, ast::TexelFormat::kR32Uint,
                                  ast::Access::kWrite);

    GlobalVar("a", st, Group(0), Binding(0));

    if (params.is_valid) {
        EXPECT_TRUE(r()->Resolve()) << r()->error();
    } else {
        EXPECT_FALSE(r()->Resolve());
        EXPECT_EQ(r()->error(),
                  "12:34 error: cube dimensions for storage textures are not supported");
    }
}
INSTANTIATE_TEST_SUITE_P(ResolverTypeValidationTest,
                         StorageTextureDimensionTest,
                         testing::ValuesIn(Dimension_cases));

struct FormatParams {
    ast::TexelFormat format;
    bool is_valid;
};

static constexpr FormatParams format_cases[] = {FormatParams{ast::TexelFormat::kR32Float, true},
                                                FormatParams{ast::TexelFormat::kR32Sint, true},
                                                FormatParams{ast::TexelFormat::kR32Uint, true},
                                                FormatParams{ast::TexelFormat::kRg32Float, true},
                                                FormatParams{ast::TexelFormat::kRg32Sint, true},
                                                FormatParams{ast::TexelFormat::kRg32Uint, true},
                                                FormatParams{ast::TexelFormat::kRgba16Float, true},
                                                FormatParams{ast::TexelFormat::kRgba16Sint, true},
                                                FormatParams{ast::TexelFormat::kRgba16Uint, true},
                                                FormatParams{ast::TexelFormat::kRgba32Float, true},
                                                FormatParams{ast::TexelFormat::kRgba32Sint, true},
                                                FormatParams{ast::TexelFormat::kRgba32Uint, true},
                                                FormatParams{ast::TexelFormat::kRgba8Sint, true},
                                                FormatParams{ast::TexelFormat::kRgba8Snorm, true},
                                                FormatParams{ast::TexelFormat::kRgba8Uint, true},
                                                FormatParams{ast::TexelFormat::kRgba8Unorm, true}};

using StorageTextureFormatTest = ResolverTestWithParam<FormatParams>;
TEST_P(StorageTextureFormatTest, All) {
    auto& params = GetParam();
    // @group(0) @binding(0)
    // var a : texture_storage_1d<*, write>;
    // @group(0) @binding(1)
    // var b : texture_storage_2d<*, write>;
    // @group(0) @binding(2)
    // var c : texture_storage_2d_array<*, write>;
    // @group(0) @binding(3)
    // var d : texture_storage_3d<*, write>;

    auto* st_a = ty.storage_texture(Source{{12, 34}}, ast::TextureDimension::k1d, params.format,
                                    ast::Access::kWrite);
    GlobalVar("a", st_a, Group(0), Binding(0));

    auto* st_b = ty.storage_texture(ast::TextureDimension::k2d, params.format, ast::Access::kWrite);
    GlobalVar("b", st_b, Group(0), Binding(1));

    auto* st_c =
        ty.storage_texture(ast::TextureDimension::k2dArray, params.format, ast::Access::kWrite);
    GlobalVar("c", st_c, Group(0), Binding(2));

    auto* st_d = ty.storage_texture(ast::TextureDimension::k3d, params.format, ast::Access::kWrite);
    GlobalVar("d", st_d, Group(0), Binding(3));

    if (params.is_valid) {
        EXPECT_TRUE(r()->Resolve()) << r()->error();
    } else {
        EXPECT_FALSE(r()->Resolve());
        EXPECT_EQ(r()->error(),
                  "12:34 error: image format must be one of the texel formats specified for "
                  "storage textues in https://gpuweb.github.io/gpuweb/wgsl/#texel-formats");
    }
}
INSTANTIATE_TEST_SUITE_P(ResolverTypeValidationTest,
                         StorageTextureFormatTest,
                         testing::ValuesIn(format_cases));

using StorageTextureAccessTest = ResolverTest;

TEST_F(StorageTextureAccessTest, MissingAccess_Fail) {
    // @group(0) @binding(0)
    // var a : texture_storage_1d<ru32int>;

    auto* st = ty.storage_texture(Source{{12, 34}}, ast::TextureDimension::k1d,
                                  ast::TexelFormat::kR32Uint, ast::Access::kUndefined);

    GlobalVar("a", st, Group(0), Binding(0));

    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(), "12:34 error: storage texture missing access control");
}

TEST_F(StorageTextureAccessTest, RWAccess_Fail) {
    // @group(0) @binding(0)
    // var a : texture_storage_1d<ru32int, read_write>;

    auto* st = ty.storage_texture(Source{{12, 34}}, ast::TextureDimension::k1d,
                                  ast::TexelFormat::kR32Uint, ast::Access::kReadWrite);

    GlobalVar("a", st, Group(0), Binding(0));

    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              "12:34 error: storage textures currently only support 'write' access control");
}

TEST_F(StorageTextureAccessTest, ReadOnlyAccess_Fail) {
    // @group(0) @binding(0)
    // var a : texture_storage_1d<ru32int, read>;

    auto* st = ty.storage_texture(Source{{12, 34}}, ast::TextureDimension::k1d,
                                  ast::TexelFormat::kR32Uint, ast::Access::kRead);

    GlobalVar("a", st, Group(0), Binding(0));

    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              "12:34 error: storage textures currently only support 'write' access control");
}

TEST_F(StorageTextureAccessTest, WriteOnlyAccess_Pass) {
    // @group(0) @binding(0)
    // var a : texture_storage_1d<ru32int, write>;

    auto* st = ty.storage_texture(ast::TextureDimension::k1d, ast::TexelFormat::kR32Uint,
                                  ast::Access::kWrite);

    GlobalVar("a", st, Group(0), Binding(0));

    EXPECT_TRUE(r()->Resolve()) << r()->error();
}

}  // namespace StorageTextureTests

namespace MatrixTests {
struct Params {
    uint32_t columns;
    uint32_t rows;
    builder::ast_type_func_ptr elem_ty;
};

template <typename T>
constexpr Params ParamsFor(uint32_t columns, uint32_t rows) {
    return Params{columns, rows, DataType<T>::AST};
}

using ValidMatrixTypes = ResolverTestWithParam<Params>;
TEST_P(ValidMatrixTypes, Okay) {
    // var a : matNxM<EL_TY>;
    auto& params = GetParam();

    Enable(ast::Extension::kF16);

    GlobalVar("a", ty.mat(params.elem_ty(*this), params.columns, params.rows),
              ast::StorageClass::kPrivate);
    EXPECT_TRUE(r()->Resolve()) << r()->error();
}
INSTANTIATE_TEST_SUITE_P(ResolverTypeValidationTest,
                         ValidMatrixTypes,
                         testing::Values(ParamsFor<f32>(2, 2),
                                         ParamsFor<f32>(2, 3),
                                         ParamsFor<f32>(2, 4),
                                         ParamsFor<f32>(3, 2),
                                         ParamsFor<f32>(3, 3),
                                         ParamsFor<f32>(3, 4),
                                         ParamsFor<f32>(4, 2),
                                         ParamsFor<f32>(4, 3),
                                         ParamsFor<f32>(4, 4),
                                         ParamsFor<alias<f32>>(4, 2),
                                         ParamsFor<alias<f32>>(4, 3),
                                         ParamsFor<alias<f32>>(4, 4),
                                         ParamsFor<f16>(2, 2),
                                         ParamsFor<f16>(2, 3),
                                         ParamsFor<f16>(2, 4),
                                         ParamsFor<f16>(3, 2),
                                         ParamsFor<f16>(3, 3),
                                         ParamsFor<f16>(3, 4),
                                         ParamsFor<f16>(4, 2),
                                         ParamsFor<f16>(4, 3),
                                         ParamsFor<f16>(4, 4),
                                         ParamsFor<alias<f16>>(4, 2),
                                         ParamsFor<alias<f16>>(4, 3),
                                         ParamsFor<alias<f16>>(4, 4)));

using InvalidMatrixElementTypes = ResolverTestWithParam<Params>;
TEST_P(InvalidMatrixElementTypes, InvalidElementType) {
    // var a : matNxM<EL_TY>;
    auto& params = GetParam();

    Enable(ast::Extension::kF16);

    GlobalVar("a", ty.mat(Source{{12, 34}}, params.elem_ty(*this), params.columns, params.rows),
              ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(), "12:34 error: matrix element type must be 'f32' or 'f16'");
}
INSTANTIATE_TEST_SUITE_P(ResolverTypeValidationTest,
                         InvalidMatrixElementTypes,
                         testing::Values(ParamsFor<bool>(4, 2),
                                         ParamsFor<i32>(4, 3),
                                         ParamsFor<u32>(4, 4),
                                         ParamsFor<vec2<f32>>(2, 2),
                                         ParamsFor<vec2<f16>>(2, 2),
                                         ParamsFor<vec3<i32>>(2, 3),
                                         ParamsFor<vec4<u32>>(2, 4),
                                         ParamsFor<mat2x2<f32>>(3, 2),
                                         ParamsFor<mat3x3<f32>>(3, 3),
                                         ParamsFor<mat4x4<f32>>(3, 4),
                                         ParamsFor<mat2x2<f16>>(3, 2),
                                         ParamsFor<mat3x3<f16>>(3, 3),
                                         ParamsFor<mat4x4<f16>>(3, 4),
                                         ParamsFor<array<2, f32>>(4, 2),
                                         ParamsFor<array<2, f16>>(4, 2)));
}  // namespace MatrixTests

namespace VectorTests {
struct Params {
    uint32_t width;
    builder::ast_type_func_ptr elem_ty;
};

template <typename T>
constexpr Params ParamsFor(uint32_t width) {
    return Params{width, DataType<T>::AST};
}

using ValidVectorTypes = ResolverTestWithParam<Params>;
TEST_P(ValidVectorTypes, Okay) {
    // var a : vecN<EL_TY>;
    auto& params = GetParam();

    Enable(ast::Extension::kF16);

    GlobalVar("a", ty.vec(params.elem_ty(*this), params.width), ast::StorageClass::kPrivate);
    EXPECT_TRUE(r()->Resolve()) << r()->error();
}
INSTANTIATE_TEST_SUITE_P(ResolverTypeValidationTest,
                         ValidVectorTypes,
                         testing::Values(ParamsFor<bool>(2),
                                         ParamsFor<f32>(2),
                                         ParamsFor<f16>(2),
                                         ParamsFor<i32>(2),
                                         ParamsFor<u32>(2),
                                         ParamsFor<bool>(3),
                                         ParamsFor<f32>(3),
                                         ParamsFor<f16>(3),
                                         ParamsFor<i32>(3),
                                         ParamsFor<u32>(3),
                                         ParamsFor<bool>(4),
                                         ParamsFor<f32>(4),
                                         ParamsFor<f16>(4),
                                         ParamsFor<i32>(4),
                                         ParamsFor<u32>(4),
                                         ParamsFor<alias<bool>>(4),
                                         ParamsFor<alias<f32>>(4),
                                         ParamsFor<alias<f16>>(4),
                                         ParamsFor<alias<i32>>(4),
                                         ParamsFor<alias<u32>>(4)));

using InvalidVectorElementTypes = ResolverTestWithParam<Params>;
TEST_P(InvalidVectorElementTypes, InvalidElementType) {
    // var a : vecN<EL_TY>;
    auto& params = GetParam();

    Enable(ast::Extension::kF16);

    GlobalVar("a", ty.vec(Source{{12, 34}}, params.elem_ty(*this), params.width),
              ast::StorageClass::kPrivate);
    EXPECT_FALSE(r()->Resolve());
    EXPECT_EQ(r()->error(),
              "12:34 error: vector element type must be 'bool', 'f32', 'f16', 'i32' "
              "or 'u32'");
}
INSTANTIATE_TEST_SUITE_P(ResolverTypeValidationTest,
                         InvalidVectorElementTypes,
                         testing::Values(ParamsFor<vec2<f32>>(2),
                                         ParamsFor<vec3<i32>>(2),
                                         ParamsFor<vec4<u32>>(2),
                                         ParamsFor<mat2x2<f32>>(2),
                                         ParamsFor<mat3x3<f16>>(2),
                                         ParamsFor<mat4x4<f32>>(2),
                                         ParamsFor<array<2, f32>>(2)));
}  // namespace VectorTests

}  // namespace
}  // namespace tint::resolver
