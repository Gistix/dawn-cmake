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

#include "gmock/gmock.h"
#include "src/tint/ast/stage_attribute.h"
#include "src/tint/writer/hlsl/test_helper.h"

using ::testing::HasSubstr;

using namespace tint::number_suffixes;  // NOLINT

namespace tint::writer::hlsl {
namespace {

using create_type_func_ptr = const ast::Type* (*)(const ProgramBuilder::TypesBuilder& ty);

inline const ast::Type* ty_i32(const ProgramBuilder::TypesBuilder& ty) {
    return ty.i32();
}
inline const ast::Type* ty_u32(const ProgramBuilder::TypesBuilder& ty) {
    return ty.u32();
}
inline const ast::Type* ty_f32(const ProgramBuilder::TypesBuilder& ty) {
    return ty.f32();
}
template <typename T>
inline const ast::Type* ty_vec2(const ProgramBuilder::TypesBuilder& ty) {
    return ty.vec2<T>();
}
template <typename T>
inline const ast::Type* ty_vec3(const ProgramBuilder::TypesBuilder& ty) {
    return ty.vec3<T>();
}
template <typename T>
inline const ast::Type* ty_vec4(const ProgramBuilder::TypesBuilder& ty) {
    return ty.vec4<T>();
}
template <typename T>
inline const ast::Type* ty_mat2x2(const ProgramBuilder::TypesBuilder& ty) {
    return ty.mat2x2<T>();
}
template <typename T>
inline const ast::Type* ty_mat2x3(const ProgramBuilder::TypesBuilder& ty) {
    return ty.mat2x3<T>();
}
template <typename T>
inline const ast::Type* ty_mat2x4(const ProgramBuilder::TypesBuilder& ty) {
    return ty.mat2x4<T>();
}
template <typename T>
inline const ast::Type* ty_mat3x2(const ProgramBuilder::TypesBuilder& ty) {
    return ty.mat3x2<T>();
}
template <typename T>
inline const ast::Type* ty_mat3x3(const ProgramBuilder::TypesBuilder& ty) {
    return ty.mat3x3<T>();
}
template <typename T>
inline const ast::Type* ty_mat3x4(const ProgramBuilder::TypesBuilder& ty) {
    return ty.mat3x4<T>();
}
template <typename T>
inline const ast::Type* ty_mat4x2(const ProgramBuilder::TypesBuilder& ty) {
    return ty.mat4x2<T>();
}
template <typename T>
inline const ast::Type* ty_mat4x3(const ProgramBuilder::TypesBuilder& ty) {
    return ty.mat4x3<T>();
}
template <typename T>
inline const ast::Type* ty_mat4x4(const ProgramBuilder::TypesBuilder& ty) {
    return ty.mat4x4<T>();
}

template <typename BASE>
class HlslGeneratorImplTest_MemberAccessorBase : public BASE {
  public:
    void SetupStorageBuffer(utils::VectorRef<const ast::StructMember*> members) {
        ProgramBuilder& b = *this;
        auto* s = b.Structure("Data", members);

        b.GlobalVar("data", b.ty.Of(s), ast::StorageClass::kStorage, ast::Access::kReadWrite,
                    b.Group(1), b.Binding(0));
    }

    void SetupFunction(utils::VectorRef<const ast::Statement*> statements) {
        ProgramBuilder& b = *this;
        utils::Vector attrs{
            b.Stage(ast::PipelineStage::kFragment),
        };
        b.Func("main", utils::Empty, b.ty.void_(), std::move(statements), std::move(attrs));
    }
};

using HlslGeneratorImplTest_MemberAccessor = HlslGeneratorImplTest_MemberAccessorBase<TestHelper>;

template <typename T>
using HlslGeneratorImplTest_MemberAccessorWithParam =
    HlslGeneratorImplTest_MemberAccessorBase<TestParamHelper<T>>;

TEST_F(HlslGeneratorImplTest_MemberAccessor, EmitExpression_MemberAccessor) {
    auto* s = Structure("Data", utils::Vector{Member("mem", ty.f32())});
    GlobalVar("str", ty.Of(s), ast::StorageClass::kPrivate);

    auto* expr = MemberAccessor("str", "mem");
    WrapInFunction(Var("expr", ty.f32(), expr));

    GeneratorImpl& gen = SanitizeAndBuild();

    ASSERT_TRUE(gen.Generate()) << gen.error();
    EXPECT_EQ(gen.result(), R"(struct Data {
  float mem;
};

static Data str = (Data)0;

[numthreads(1, 1, 1)]
void test_function() {
  float expr = str.mem;
  return;
}
)");
}

struct TypeCase {
    create_type_func_ptr member_type;
    std::string expected;
};
inline std::ostream& operator<<(std::ostream& out, TypeCase c) {
    ProgramBuilder b;
    auto* ty = c.member_type(b.ty);
    out << ty->FriendlyName(b.Symbols());
    return out;
}

using HlslGeneratorImplTest_MemberAccessor_StorageBufferLoad =
    HlslGeneratorImplTest_MemberAccessorWithParam<TypeCase>;
TEST_P(HlslGeneratorImplTest_MemberAccessor_StorageBufferLoad, Test) {
    // struct Data {
    //   a : i32;
    //   b : <type>;
    // };
    // var<storage> data : Data;
    // data.b;

    auto p = GetParam();

    SetupStorageBuffer(utils::Vector{
        Member("a", ty.i32()),
        Member("b", p.member_type(ty)),
    });

    SetupFunction(utils::Vector{
        Decl(Var("x", MemberAccessor("data", "b"))),
    });

    GeneratorImpl& gen = SanitizeAndBuild();

    ASSERT_TRUE(gen.Generate()) << gen.error();
    EXPECT_THAT(gen.result(), HasSubstr(p.expected));
}

INSTANTIATE_TEST_SUITE_P(
    HlslGeneratorImplTest_MemberAccessor,
    HlslGeneratorImplTest_MemberAccessor_StorageBufferLoad,
    testing::Values(
        TypeCase{ty_u32, "data.Load(4u)"},
        TypeCase{ty_f32, "asfloat(data.Load(4u))"},
        TypeCase{ty_i32, "asint(data.Load(4u))"},
        TypeCase{ty_vec2<u32>, "data.Load2(8u)"},
        TypeCase{ty_vec2<f32>, "asfloat(data.Load2(8u))"},
        TypeCase{ty_vec2<i32>, "asint(data.Load2(8u))"},
        TypeCase{ty_vec3<u32>, "data.Load3(16u)"},
        TypeCase{ty_vec3<f32>, "asfloat(data.Load3(16u))"},
        TypeCase{ty_vec3<i32>, "asint(data.Load3(16u))"},
        TypeCase{ty_vec4<u32>, "data.Load4(16u)"},
        TypeCase{ty_vec4<f32>, "asfloat(data.Load4(16u))"},
        TypeCase{ty_vec4<i32>, "asint(data.Load4(16u))"},
        TypeCase{
            ty_mat2x2<f32>,
            R"(return float2x2(asfloat(buffer.Load2((offset + 0u))), asfloat(buffer.Load2((offset + 8u))));)"},
        TypeCase{
            ty_mat2x3<f32>,
            R"(return float2x3(asfloat(buffer.Load3((offset + 0u))), asfloat(buffer.Load3((offset + 16u))));)"},
        TypeCase{
            ty_mat2x4<f32>,
            R"(return float2x4(asfloat(buffer.Load4((offset + 0u))), asfloat(buffer.Load4((offset + 16u))));)"},
        TypeCase{
            ty_mat3x2<f32>,
            R"(return float3x2(asfloat(buffer.Load2((offset + 0u))), asfloat(buffer.Load2((offset + 8u))), asfloat(buffer.Load2((offset + 16u))));)"},
        TypeCase{
            ty_mat3x3<f32>,
            R"(return float3x3(asfloat(buffer.Load3((offset + 0u))), asfloat(buffer.Load3((offset + 16u))), asfloat(buffer.Load3((offset + 32u))));)"},
        TypeCase{
            ty_mat3x4<f32>,
            R"(return float3x4(asfloat(buffer.Load4((offset + 0u))), asfloat(buffer.Load4((offset + 16u))), asfloat(buffer.Load4((offset + 32u))));)"},
        TypeCase{
            ty_mat4x2<f32>,
            R"(return float4x2(asfloat(buffer.Load2((offset + 0u))), asfloat(buffer.Load2((offset + 8u))), asfloat(buffer.Load2((offset + 16u))), asfloat(buffer.Load2((offset + 24u))));)"},
        TypeCase{
            ty_mat4x3<f32>,
            R"(return float4x3(asfloat(buffer.Load3((offset + 0u))), asfloat(buffer.Load3((offset + 16u))), asfloat(buffer.Load3((offset + 32u))), asfloat(buffer.Load3((offset + 48u))));)"},
        TypeCase{
            ty_mat4x4<f32>,
            R"(return float4x4(asfloat(buffer.Load4((offset + 0u))), asfloat(buffer.Load4((offset + 16u))), asfloat(buffer.Load4((offset + 32u))), asfloat(buffer.Load4((offset + 48u))));)"}));

using HlslGeneratorImplTest_MemberAccessor_StorageBufferStore =
    HlslGeneratorImplTest_MemberAccessorWithParam<TypeCase>;
TEST_P(HlslGeneratorImplTest_MemberAccessor_StorageBufferStore, Test) {
    // struct Data {
    //   a : i32;
    //   b : <type>;
    // };
    // var<storage> data : Data;
    // data.b = <type>();

    auto p = GetParam();

    SetupStorageBuffer(utils::Vector{
        Member("a", ty.i32()),
        Member("b", p.member_type(ty)),
    });

    SetupFunction(utils::Vector{
        Decl(Var("value", p.member_type(ty), Construct(p.member_type(ty)))),
        Assign(MemberAccessor("data", "b"), Expr("value")),
    });

    GeneratorImpl& gen = SanitizeAndBuild();

    ASSERT_TRUE(gen.Generate()) << gen.error();
    EXPECT_THAT(gen.result(), HasSubstr(p.expected));
}

INSTANTIATE_TEST_SUITE_P(HlslGeneratorImplTest_MemberAccessor,
                         HlslGeneratorImplTest_MemberAccessor_StorageBufferStore,
                         testing::Values(TypeCase{ty_u32, "data.Store(4u, asuint(value))"},
                                         TypeCase{ty_f32, "data.Store(4u, asuint(value))"},
                                         TypeCase{ty_i32, "data.Store(4u, asuint(value))"},
                                         TypeCase{ty_vec2<u32>, "data.Store2(8u, asuint(value))"},
                                         TypeCase{ty_vec2<f32>, "data.Store2(8u, asuint(value))"},
                                         TypeCase{ty_vec2<i32>, "data.Store2(8u, asuint(value))"},
                                         TypeCase{ty_vec3<u32>, "data.Store3(16u, asuint(value))"},
                                         TypeCase{ty_vec3<f32>, "data.Store3(16u, asuint(value))"},
                                         TypeCase{ty_vec3<i32>, "data.Store3(16u, asuint(value))"},
                                         TypeCase{ty_vec4<u32>, "data.Store4(16u, asuint(value))"},
                                         TypeCase{ty_vec4<f32>, "data.Store4(16u, asuint(value))"},
                                         TypeCase{ty_vec4<i32>, "data.Store4(16u, asuint(value))"},
                                         TypeCase{ty_mat2x2<f32>, R"({
  buffer.Store2((offset + 0u), asuint(value[0u]));
  buffer.Store2((offset + 8u), asuint(value[1u]));
})"},
                                         TypeCase{ty_mat2x3<f32>, R"({
  buffer.Store3((offset + 0u), asuint(value[0u]));
  buffer.Store3((offset + 16u), asuint(value[1u]));
})"},
                                         TypeCase{ty_mat2x4<f32>, R"({
  buffer.Store4((offset + 0u), asuint(value[0u]));
  buffer.Store4((offset + 16u), asuint(value[1u]));
})"},
                                         TypeCase{ty_mat3x2<f32>, R"({
  buffer.Store2((offset + 0u), asuint(value[0u]));
  buffer.Store2((offset + 8u), asuint(value[1u]));
  buffer.Store2((offset + 16u), asuint(value[2u]));
})"},
                                         TypeCase{ty_mat3x3<f32>, R"({
  buffer.Store3((offset + 0u), asuint(value[0u]));
  buffer.Store3((offset + 16u), asuint(value[1u]));
  buffer.Store3((offset + 32u), asuint(value[2u]));
})"},
                                         TypeCase{ty_mat3x4<f32>, R"({
  buffer.Store4((offset + 0u), asuint(value[0u]));
  buffer.Store4((offset + 16u), asuint(value[1u]));
  buffer.Store4((offset + 32u), asuint(value[2u]));
})"},
                                         TypeCase{ty_mat4x2<f32>, R"({
  buffer.Store2((offset + 0u), asuint(value[0u]));
  buffer.Store2((offset + 8u), asuint(value[1u]));
  buffer.Store2((offset + 16u), asuint(value[2u]));
  buffer.Store2((offset + 24u), asuint(value[3u]));
})"},
                                         TypeCase{ty_mat4x3<f32>, R"({
  buffer.Store3((offset + 0u), asuint(value[0u]));
  buffer.Store3((offset + 16u), asuint(value[1u]));
  buffer.Store3((offset + 32u), asuint(value[2u]));
  buffer.Store3((offset + 48u), asuint(value[3u]));
})"},
                                         TypeCase{ty_mat4x4<f32>, R"({
  buffer.Store4((offset + 0u), asuint(value[0u]));
  buffer.Store4((offset + 16u), asuint(value[1u]));
  buffer.Store4((offset + 32u), asuint(value[2u]));
  buffer.Store4((offset + 48u), asuint(value[3u]));
})"}));

TEST_F(HlslGeneratorImplTest_MemberAccessor, StorageBuffer_Store_Matrix_Empty) {
    // struct Data {
    //   z : f32;
    //   a : mat2x3<f32>;
    // };
    // var<storage> data : Data;
    // data.a = mat2x3<f32>();

    SetupStorageBuffer(utils::Vector{
        Member("a", ty.i32()),
        Member("b", ty.mat2x3<f32>()),
    });

    SetupFunction(utils::Vector{
        Assign(MemberAccessor("data", "b"), Construct(ty.mat2x3<f32>())),
    });

    GeneratorImpl& gen = SanitizeAndBuild();

    ASSERT_TRUE(gen.Generate()) << gen.error();
    auto* expected =
        R"(RWByteAddressBuffer data : register(u0, space1);

void tint_symbol(RWByteAddressBuffer buffer, uint offset, float2x3 value) {
  buffer.Store3((offset + 0u), asuint(value[0u]));
  buffer.Store3((offset + 16u), asuint(value[1u]));
}

void main() {
  tint_symbol(data, 16u, float2x3((0.0f).xxx, (0.0f).xxx));
  return;
}
)";
    EXPECT_EQ(gen.result(), expected);
}

TEST_F(HlslGeneratorImplTest_MemberAccessor, StorageBuffer_Load_Matrix_Single_Element) {
    // struct Data {
    //   z : f32;
    //   a : mat4x3<f32>;
    // };
    // var<storage> data : Data;
    // data.a[2i][1i];

    SetupStorageBuffer(utils::Vector{
        Member("z", ty.f32()),
        Member("a", ty.mat4x3<f32>()),
    });

    SetupFunction(utils::Vector{
        Decl(Var("x", IndexAccessor(IndexAccessor(MemberAccessor("data", "a"), 2_i), 1_i))),
    });

    GeneratorImpl& gen = SanitizeAndBuild();

    ASSERT_TRUE(gen.Generate()) << gen.error();
    auto* expected =
        R"(RWByteAddressBuffer data : register(u0, space1);

void main() {
  float x = asfloat(data.Load(52u));
  return;
}
)";
    EXPECT_EQ(gen.result(), expected);
}

TEST_F(HlslGeneratorImplTest_MemberAccessor,
       EmitExpression_IndexAccessor_StorageBuffer_Load_Int_FromArray) {
    // struct Data {
    //   a : array<i32, 5>;
    // };
    // var<storage> data : Data;
    // data.a[2];

    SetupStorageBuffer(utils::Vector{
        Member("z", ty.f32()),
        Member("a", ty.array<i32, 5>(4)),
    });

    SetupFunction(utils::Vector{
        Decl(Var("x", IndexAccessor(MemberAccessor("data", "a"), 2_i))),
    });

    GeneratorImpl& gen = SanitizeAndBuild();

    ASSERT_TRUE(gen.Generate()) << gen.error();
    auto* expected =
        R"(RWByteAddressBuffer data : register(u0, space1);

void main() {
  int x = asint(data.Load(12u));
  return;
}
)";
    EXPECT_EQ(gen.result(), expected);
}

TEST_F(HlslGeneratorImplTest_MemberAccessor,
       EmitExpression_IndexAccessor_StorageBuffer_Load_Int_FromArray_ExprIdx) {
    // struct Data {
    //   a : array<i32, 5>;
    // };
    // var<storage> data : Data;
    // data.a[(2i + 4i) - 3i];

    SetupStorageBuffer(utils::Vector{
        Member("z", ty.f32()),
        Member("a", ty.array<i32, 5>(4)),
    });

    SetupFunction(utils::Vector{
        Decl(Var("a", Expr(2_i))),
        Decl(Var("b", Expr(4_i))),
        Decl(Var("c", Expr(3_i))),
        Decl(Var("x", IndexAccessor(MemberAccessor("data", "a"), Sub(Add("a", "b"), "c")))),
    });

    GeneratorImpl& gen = SanitizeAndBuild();

    ASSERT_TRUE(gen.Generate()) << gen.error();
    auto* expected =
        R"(RWByteAddressBuffer data : register(u0, space1);

void main() {
  int a = 2;
  int b = 4;
  int c = 3;
  int x = asint(data.Load((4u + (4u * uint(((a + b) - c))))));
  return;
}
)";
    EXPECT_EQ(gen.result(), expected);
}

TEST_F(HlslGeneratorImplTest_MemberAccessor, StorageBuffer_Store_ToArray) {
    // struct Data {
    //   a : array<i32, 5>;
    // };
    // var<storage> data : Data;
    // data.a[2] = 2;

    SetupStorageBuffer(utils::Vector{
        Member("z", ty.f32()),
        Member("a", ty.array<i32, 5>(4)),
    });

    SetupFunction(utils::Vector{
        Assign(IndexAccessor(MemberAccessor("data", "a"), 2_i), 2_i),
    });

    GeneratorImpl& gen = SanitizeAndBuild();

    ASSERT_TRUE(gen.Generate()) << gen.error();
    auto* expected =
        R"(RWByteAddressBuffer data : register(u0, space1);

void main() {
  data.Store(12u, asuint(2));
  return;
}
)";
    EXPECT_EQ(gen.result(), expected);
}

TEST_F(HlslGeneratorImplTest_MemberAccessor, StorageBuffer_Load_MultiLevel) {
    // struct Inner {
    //   a : vec3<i32>;
    //   b : vec3<f32>;
    // };
    // struct Data {
    //   var c : array<Inner, 4u>;
    // };
    //
    // var<storage> data : Pre;
    // data.c[2].b

    auto* inner = Structure("Inner", utils::Vector{
                                         Member("a", ty.vec3<f32>()),
                                         Member("b", ty.vec3<f32>()),
                                     });

    SetupStorageBuffer(utils::Vector{
        Member("c", ty.array(ty.Of(inner), 4_u, 32)),
    });

    SetupFunction(utils::Vector{
        Decl(Var("x", MemberAccessor(IndexAccessor(MemberAccessor("data", "c"), 2_i), "b"))),
    });

    GeneratorImpl& gen = SanitizeAndBuild();

    ASSERT_TRUE(gen.Generate()) << gen.error();
    auto* expected =
        R"(RWByteAddressBuffer data : register(u0, space1);

void main() {
  float3 x = asfloat(data.Load3(80u));
  return;
}
)";
    EXPECT_EQ(gen.result(), expected);
}

TEST_F(HlslGeneratorImplTest_MemberAccessor, StorageBuffer_Load_MultiLevel_Swizzle) {
    // struct Inner {
    //   a : vec3<i32>;
    //   b : vec3<f32>;
    // };
    // struct Data {
    //   var c : array<Inner, 4u>;
    // };
    //
    // var<storage> data : Pre;
    // data.c[2].b.xy

    auto* inner = Structure("Inner", utils::Vector{
                                         Member("a", ty.vec3<f32>()),
                                         Member("b", ty.vec3<f32>()),
                                     });

    SetupStorageBuffer(utils::Vector{
        Member("c", ty.array(ty.Of(inner), 4_u, 32)),
    });

    SetupFunction(utils::Vector{
        Decl(Var("x",
                 MemberAccessor(
                     MemberAccessor(IndexAccessor(MemberAccessor("data", "c"), 2_i), "b"), "xy"))),
    });

    GeneratorImpl& gen = SanitizeAndBuild();

    ASSERT_TRUE(gen.Generate()) << gen.error();
    auto* expected =
        R"(RWByteAddressBuffer data : register(u0, space1);

void main() {
  float2 x = asfloat(data.Load3(80u)).xy;
  return;
}
)";
    EXPECT_EQ(gen.result(), expected);
}

TEST_F(HlslGeneratorImplTest_MemberAccessor,
       StorageBuffer_Load_MultiLevel_Swizzle_SingleLetter) {  // NOLINT
    // struct Inner {
    //   a : vec3<i32>;
    //   b : vec3<f32>;
    // };
    // struct Data {
    //   var c : array<Inner, 4u>;
    // };
    //
    // var<storage> data : Pre;
    // data.c[2].b.g

    auto* inner = Structure("Inner", utils::Vector{
                                         Member("a", ty.vec3<f32>()),
                                         Member("b", ty.vec3<f32>()),
                                     });

    SetupStorageBuffer(utils::Vector{
        Member("c", ty.array(ty.Of(inner), 4_u, 32)),
    });

    SetupFunction(utils::Vector{
        Decl(Var("x",
                 MemberAccessor(
                     MemberAccessor(IndexAccessor(MemberAccessor("data", "c"), 2_i), "b"), "g"))),
    });

    GeneratorImpl& gen = SanitizeAndBuild();

    ASSERT_TRUE(gen.Generate()) << gen.error();
    auto* expected =
        R"(RWByteAddressBuffer data : register(u0, space1);

void main() {
  float x = asfloat(data.Load(84u));
  return;
}
)";
    EXPECT_EQ(gen.result(), expected);
}

TEST_F(HlslGeneratorImplTest_MemberAccessor, StorageBuffer_Load_MultiLevel_Index) {
    // struct Inner {
    //   a : vec3<i32>;
    //   b : vec3<f32>;
    // };
    // struct Data {
    //   var c : array<Inner, 4u>;
    // };
    //
    // var<storage> data : Pre;
    // data.c[2].b[1]

    auto* inner = Structure("Inner", utils::Vector{
                                         Member("a", ty.vec3<f32>()),
                                         Member("b", ty.vec3<f32>()),
                                     });

    SetupStorageBuffer(utils::Vector{
        Member("c", ty.array(ty.Of(inner), 4_u, 32)),
    });

    SetupFunction(utils::Vector{
        Decl(Var("x",
                 IndexAccessor(MemberAccessor(IndexAccessor(MemberAccessor("data", "c"), 2_i), "b"),
                               1_i))),
    });

    GeneratorImpl& gen = SanitizeAndBuild();

    ASSERT_TRUE(gen.Generate()) << gen.error();
    auto* expected =
        R"(RWByteAddressBuffer data : register(u0, space1);

void main() {
  float x = asfloat(data.Load(84u));
  return;
}
)";
    EXPECT_EQ(gen.result(), expected);
}

TEST_F(HlslGeneratorImplTest_MemberAccessor, StorageBuffer_Store_MultiLevel) {
    // struct Inner {
    //   a : vec3<i32>;
    //   b : vec3<f32>;
    // };
    // struct Data {
    //   var c : array<Inner, 4u>;
    // };
    //
    // var<storage> data : Pre;
    // data.c[2].b = vec3<f32>(1_f, 2_f, 3_f);

    auto* inner = Structure("Inner", utils::Vector{
                                         Member("a", ty.vec3<f32>()),
                                         Member("b", ty.vec3<f32>()),
                                     });

    SetupStorageBuffer(utils::Vector{
        Member("c", ty.array(ty.Of(inner), 4_u, 32)),
    });

    SetupFunction(utils::Vector{
        Assign(MemberAccessor(IndexAccessor(MemberAccessor("data", "c"), 2_i), "b"),
               vec3<f32>(1_f, 2_f, 3_f)),
    });

    GeneratorImpl& gen = SanitizeAndBuild();

    ASSERT_TRUE(gen.Generate()) << gen.error();
    auto* expected =
        R"(RWByteAddressBuffer data : register(u0, space1);

void main() {
  data.Store3(80u, asuint(float3(1.0f, 2.0f, 3.0f)));
  return;
}
)";
    EXPECT_EQ(gen.result(), expected);
}

TEST_F(HlslGeneratorImplTest_MemberAccessor, StorageBuffer_Store_Swizzle_SingleLetter) {
    // struct Inner {
    //   a : vec3<i32>;
    //   b : vec3<f32>;
    // };
    // struct Data {
    //   var c : array<Inner, 4u>;
    // };
    //
    // var<storage> data : Pre;
    // data.c[2].b.y = 1.f;

    auto* inner = Structure("Inner", utils::Vector{
                                         Member("a", ty.vec3<i32>()),
                                         Member("b", ty.vec3<f32>()),
                                     });

    SetupStorageBuffer(utils::Vector{
        Member("c", ty.array(ty.Of(inner), 4_u, 32)),
    });

    SetupFunction(utils::Vector{
        Assign(MemberAccessor(MemberAccessor(IndexAccessor(MemberAccessor("data", "c"), 2_i), "b"),
                              "y"),
               Expr(1_f)),
    });

    GeneratorImpl& gen = SanitizeAndBuild();

    ASSERT_TRUE(gen.Generate()) << gen.error();
    auto* expected =
        R"(RWByteAddressBuffer data : register(u0, space1);

void main() {
  data.Store(84u, asuint(1.0f));
  return;
}
)";
    EXPECT_EQ(gen.result(), expected);
}

TEST_F(HlslGeneratorImplTest_MemberAccessor, Swizzle_xyz) {
    auto* var = Var("my_vec", ty.vec4<f32>(), vec4<f32>(1_f, 2_f, 3_f, 4_f));
    auto* expr = MemberAccessor("my_vec", "xyz");
    WrapInFunction(var, expr);

    GeneratorImpl& gen = SanitizeAndBuild();
    ASSERT_TRUE(gen.Generate()) << gen.error();
    EXPECT_THAT(gen.result(), HasSubstr("my_vec.xyz"));
}

TEST_F(HlslGeneratorImplTest_MemberAccessor, Swizzle_gbr) {
    auto* var = Var("my_vec", ty.vec4<f32>(), vec4<f32>(1_f, 2_f, 3_f, 4_f));
    auto* expr = MemberAccessor("my_vec", "gbr");
    WrapInFunction(var, expr);

    GeneratorImpl& gen = SanitizeAndBuild();
    ASSERT_TRUE(gen.Generate()) << gen.error();
    EXPECT_THAT(gen.result(), HasSubstr("my_vec.gbr"));
}

}  // namespace
}  // namespace tint::writer::hlsl
