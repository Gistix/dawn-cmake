// Copyright 2022 The Tint Authors.
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

#include "src/tint/sem/materialize.h"

#include "src/tint/resolver/resolver.h"
#include "src/tint/resolver/resolver_test_helper.h"
#include "src/tint/sem/test_helper.h"

#include "gmock/gmock.h"

using namespace tint::number_suffixes;  // NOLINT

namespace tint::resolver {
namespace {

using AFloatV = builder::vec<3, AFloat>;
using AFloatM = builder::mat<3, 2, AFloat>;
using AFloatA = builder::array<3, AFloat>;
using AIntV = builder::vec<3, AInt>;
using AIntA = builder::array<3, AInt>;
using f32V = builder::vec<3, f32>;
using f16V = builder::vec<3, f16>;
using i32V = builder::vec<3, i32>;
using u32V = builder::vec<3, u32>;
using f32M = builder::mat<3, 2, f32>;
using f16M = builder::mat<3, 2, f16>;
using f32A = builder::array<3, f32>;
using f16A = builder::array<3, f16>;
using i32A = builder::array<3, i32>;
using u32A = builder::array<3, u32>;

constexpr double kTooBigF32 = static_cast<double>(3.5e+38);
constexpr double kTooBigF16 = static_cast<double>(6.6e+4);
constexpr double kPiF64 = 3.141592653589793;
constexpr double kPiF32 = 3.1415927410125732;  // kPiF64 quantized to f32
constexpr double kPiF16 = 3.140625;            // kPiF64 quantized to f16

constexpr double kSubnormalF32 = 0x1.0p-128;
constexpr double kSubnormalF16 = 0x1.0p-16;

enum class Expectation {
    kMaterialize,
    kNoMaterialize,
    kInvalidConversion,
    kValueCannotBeRepresented,
};

static std::ostream& operator<<(std::ostream& o, Expectation m) {
    switch (m) {
        case Expectation::kMaterialize:
            return o << "materialize";
        case Expectation::kNoMaterialize:
            return o << "no-materialize";
        case Expectation::kInvalidConversion:
            return o << "invalid-conversion";
        case Expectation::kValueCannotBeRepresented:
            return o << "value cannot be represented";
    }
    return o << "<unknown>";
}

template <typename CASE>
class MaterializeTest : public resolver::ResolverTestWithParam<CASE> {
  protected:
    using ProgramBuilder::FriendlyName;

    void CheckTypesAndValues(const sem::Expression* expr,
                             const tint::sem::Type* expected_sem_ty,
                             const std::variant<AInt, AFloat>& expected_value) {
        std::visit([&](auto v) { CheckTypesAndValuesImpl(expr, expected_sem_ty, v); },
                   expected_value);
    }

  private:
    template <typename T>
    void CheckTypesAndValuesImpl(const sem::Expression* expr,
                                 const tint::sem::Type* expected_sem_ty,
                                 T expected_value) {
        EXPECT_TYPE(expr->Type(), expected_sem_ty);

        auto* value = expr->ConstantValue();
        ASSERT_NE(value, nullptr);
        EXPECT_TYPE(expr->Type(), value->Type());

        tint::Switch(
            expected_sem_ty,  //
            [&](const sem::Vector* v) {
                for (uint32_t i = 0; i < v->Width(); i++) {
                    auto* el = value->Index(i);
                    ASSERT_NE(el, nullptr);
                    EXPECT_TYPE(el->Type(), v->type());
                    EXPECT_EQ(std::get<T>(el->Value()), expected_value);
                }
            },
            [&](const sem::Matrix* m) {
                for (uint32_t c = 0; c < m->columns(); c++) {
                    auto* column = value->Index(c);
                    ASSERT_NE(column, nullptr);
                    EXPECT_TYPE(column->Type(), m->ColumnType());
                    for (uint32_t r = 0; r < m->rows(); r++) {
                        auto* el = column->Index(r);
                        ASSERT_NE(el, nullptr);
                        EXPECT_TYPE(el->Type(), m->type());
                        EXPECT_EQ(std::get<T>(el->Value()), expected_value);
                    }
                }
            },
            [&](const sem::Array* a) {
                for (uint32_t i = 0; i < a->Count(); i++) {
                    auto* el = value->Index(i);
                    ASSERT_NE(el, nullptr);
                    EXPECT_TYPE(el->Type(), a->ElemType());
                    EXPECT_EQ(std::get<T>(el->Value()), expected_value);
                }
            },
            [&](Default) { EXPECT_EQ(std::get<T>(value->Value()), expected_value); });
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// MaterializeAbstractNumericToConcreteType
// Tests that an abstract-numeric will materialize to the expected concrete type
////////////////////////////////////////////////////////////////////////////////////////////////////
namespace materialize_abstract_numeric_to_concrete_type {
// How should the materialization occur?
enum class Method {
    // var a : target_type = abstract_expr;
    kVar,

    // let a : target_type = abstract_expr;
    kLet,

    // var a : target_type;
    // a = abstract_expr;
    kAssign,

    // _ = abstract_expr;
    kPhonyAssign,

    // fn F(v : target_type) {}
    // fn x() {
    //   F(abstract_expr);
    // }
    kFnArg,

    // min(target_expr, abstract_expr);
    kBuiltinArg,

    // fn F() : target_type {
    //   return abstract_expr;
    // }
    kReturn,

    // array<target_type, 1>(abstract_expr);
    kArray,

    // struct S {
    //   v : target_type
    // };
    // fn x() {
    //   _ = S(abstract_expr)
    // }
    kStruct,

    // target_expr + abstract_expr
    kBinaryOp,

    // switch (abstract_expr) {
    //   case target_expr: {}
    //   default: {}
    // }
    kSwitchCond,

    // switch (target_expr) {
    //   case abstract_expr: {}
    //   default: {}
    // }
    kSwitchCase,

    // switch (abstract_expr) {
    //   case 123: {}
    //   case target_expr: {}
    //   default: {}
    // }
    kSwitchCondWithAbstractCase,

    // switch (target_expr) {
    //   case 123: {}
    //   case abstract_expr: {}
    //   default: {}
    // }
    kSwitchCaseWithAbstractCase,

    // @workgroup_size(target_expr, abstract_expr, 123)
    // @compute
    // fn f() {}
    kWorkgroupSize,

    // abstract_expr[runtime-index]
    kRuntimeIndex,
};

static std::ostream& operator<<(std::ostream& o, Method m) {
    switch (m) {
        case Method::kVar:
            return o << "var";
        case Method::kLet:
            return o << "let";
        case Method::kAssign:
            return o << "assign";
        case Method::kPhonyAssign:
            return o << "phony-assign";
        case Method::kFnArg:
            return o << "fn-arg";
        case Method::kBuiltinArg:
            return o << "builtin-arg";
        case Method::kReturn:
            return o << "return";
        case Method::kArray:
            return o << "array";
        case Method::kStruct:
            return o << "struct";
        case Method::kBinaryOp:
            return o << "binary-op";
        case Method::kSwitchCond:
            return o << "switch-cond";
        case Method::kSwitchCase:
            return o << "switch-case";
        case Method::kSwitchCondWithAbstractCase:
            return o << "switch-cond-with-abstract";
        case Method::kSwitchCaseWithAbstractCase:
            return o << "switch-case-with-abstract";
        case Method::kWorkgroupSize:
            return o << "workgroup-size";
        case Method::kRuntimeIndex:
            return o << "runtime-index";
    }
    return o << "<unknown>";
}

struct Data {
    std::string target_type_name;
    std::string target_element_type_name;
    builder::ast_type_func_ptr target_ast_ty;
    builder::sem_type_func_ptr target_sem_ty;
    builder::ast_expr_func_ptr target_expr;
    std::string abstract_type_name;
    builder::ast_expr_func_ptr abstract_expr;
    std::variant<AInt, AFloat> materialized_value;
    double literal_value;
};

template <typename TARGET_TYPE, typename ABSTRACT_TYPE, typename MATERIALIZED_TYPE>
Data Types(MATERIALIZED_TYPE materialized_value, double literal_value) {
    using TargetDataType = builder::DataType<TARGET_TYPE>;
    using AbstractDataType = builder::DataType<ABSTRACT_TYPE>;
    using TargetElementDataType = builder::DataType<typename TargetDataType::ElementType>;
    return {
        TargetDataType::Name(),         // target_type_name
        TargetElementDataType::Name(),  // target_element_type_name
        TargetDataType::AST,            // target_ast_ty
        TargetDataType::Sem,            // target_sem_ty
        TargetDataType::Expr,           // target_expr
        AbstractDataType::Name(),       // abstract_type_name
        AbstractDataType::Expr,         // abstract_expr
        materialized_value,
        literal_value,
    };
}

template <typename TARGET_TYPE, typename ABSTRACT_TYPE>
Data Types() {
    using TargetDataType = builder::DataType<TARGET_TYPE>;
    using AbstractDataType = builder::DataType<ABSTRACT_TYPE>;
    using TargetElementDataType = builder::DataType<typename TargetDataType::ElementType>;
    return {
        TargetDataType::Name(),         // target_type_name
        TargetElementDataType::Name(),  // target_element_type_name
        TargetDataType::AST,            // target_ast_ty
        TargetDataType::Sem,            // target_sem_ty
        TargetDataType::Expr,           // target_expr
        AbstractDataType::Name(),       // abstract_type_name
        AbstractDataType::Expr,         // abstract_expr
        0_a,
        0.0,
    };
}

static std::ostream& operator<<(std::ostream& o, const Data& c) {
    auto print_value = [&](auto&& v) { o << v; };
    o << "[" << c.target_type_name << " <- " << c.abstract_type_name << "] [";
    std::visit(print_value, c.materialized_value);
    o << " <- " << c.literal_value << "]";
    return o;
}

using MaterializeAbstractNumericToConcreteType =
    MaterializeTest<std::tuple<Expectation, Method, Data>>;

TEST_P(MaterializeAbstractNumericToConcreteType, Test) {
    Enable(ast::Extension::kF16);

    const auto& param = GetParam();
    const auto& expectation = std::get<0>(param);
    const auto& method = std::get<1>(param);
    const auto& data = std::get<2>(param);

    auto target_ty = [&] { return data.target_ast_ty(*this); };
    auto target_expr = [&] { return data.target_expr(*this, 42); };
    auto* abstract_expr = data.abstract_expr(*this, data.literal_value);
    switch (method) {
        case Method::kVar:
            WrapInFunction(Decl(Var("a", target_ty(), abstract_expr)));
            break;
        case Method::kLet:
            WrapInFunction(Decl(Let("a", target_ty(), abstract_expr)));
            break;
        case Method::kAssign:
            WrapInFunction(Decl(Var("a", target_ty())), Assign("a", abstract_expr));
            break;
        case Method::kPhonyAssign:
            WrapInFunction(Assign(Phony(), abstract_expr));
            break;
        case Method::kFnArg:
            Func("F", utils::Vector{Param("P", target_ty())}, ty.void_(), utils::Empty);
            WrapInFunction(CallStmt(Call("F", abstract_expr)));
            break;
        case Method::kBuiltinArg:
            WrapInFunction(CallStmt(Call("min", target_expr(), abstract_expr)));
            break;
        case Method::kReturn:
            Func("F", utils::Empty, target_ty(), utils::Vector{Return(abstract_expr)});
            break;
        case Method::kArray:
            WrapInFunction(Construct(ty.array(target_ty(), 1_i), abstract_expr));
            break;
        case Method::kStruct:
            Structure("S", utils::Vector{Member("v", target_ty())});
            WrapInFunction(Construct(ty.type_name("S"), abstract_expr));
            break;
        case Method::kBinaryOp:
            WrapInFunction(Add(target_expr(), abstract_expr));
            break;
        case Method::kSwitchCond:
            WrapInFunction(Switch(abstract_expr,                                         //
                                  Case(target_expr()->As<ast::IntLiteralExpression>()),  //
                                  DefaultCase()));
            break;
        case Method::kSwitchCase:
            WrapInFunction(Switch(target_expr(),                                         //
                                  Case(abstract_expr->As<ast::IntLiteralExpression>()),  //
                                  DefaultCase()));
            break;
        case Method::kSwitchCondWithAbstractCase:
            WrapInFunction(Switch(abstract_expr,                                         //
                                  Case(Expr(123_a)),                                     //
                                  Case(target_expr()->As<ast::IntLiteralExpression>()),  //
                                  DefaultCase()));
            break;
        case Method::kSwitchCaseWithAbstractCase:
            WrapInFunction(Switch(target_expr(),                                         //
                                  Case(Expr(123_a)),                                     //
                                  Case(abstract_expr->As<ast::IntLiteralExpression>()),  //
                                  DefaultCase()));
            break;
        case Method::kWorkgroupSize:
            Func("f", utils::Empty, ty.void_(), utils::Empty,
                 utils::Vector{WorkgroupSize(target_expr(), abstract_expr, Expr(123_a)),
                               Stage(ast::PipelineStage::kCompute)});
            break;
        case Method::kRuntimeIndex:
            auto* runtime_index = Var("runtime_index", Expr(1_i));
            WrapInFunction(runtime_index, IndexAccessor(abstract_expr, runtime_index));
            break;
    }

    switch (expectation) {
        case Expectation::kMaterialize: {
            ASSERT_TRUE(r()->Resolve()) << r()->error();
            auto* materialize = Sem().Get<sem::Materialize>(abstract_expr);
            ASSERT_NE(materialize, nullptr);
            CheckTypesAndValues(materialize, data.target_sem_ty(*this), data.materialized_value);
            break;
        }
        case Expectation::kNoMaterialize: {
            ASSERT_TRUE(r()->Resolve()) << r()->error();
            auto* sem = Sem().Get(abstract_expr);
            ASSERT_NE(sem, nullptr);
            EXPECT_FALSE(sem->Is<sem::Materialize>());
            CheckTypesAndValues(sem, data.target_sem_ty(*this), data.materialized_value);
            break;
        }
        case Expectation::kInvalidConversion: {
            ASSERT_FALSE(r()->Resolve());
            std::string expect;
            switch (method) {
                case Method::kBuiltinArg:
                    expect = "error: no matching call to min(" + data.target_type_name + ", " +
                             data.abstract_type_name + ")";
                    break;
                case Method::kBinaryOp:
                    expect = "error: no matching overload for operator + (" +
                             data.target_type_name + ", " + data.abstract_type_name + ")";
                    break;
                default:
                    expect = "error: cannot convert value of type '" + data.abstract_type_name +
                             "' to type '" + data.target_type_name + "'";
                    break;
            }
            EXPECT_THAT(r()->error(), testing::StartsWith(expect));
            break;
        }
        case Expectation::kValueCannotBeRepresented:
            ASSERT_FALSE(r()->Resolve());
            EXPECT_THAT(r()->error(), testing::HasSubstr("cannot be represented as '" +
                                                         data.target_element_type_name + "'"));
            break;
    }
}

/// Methods that support scalar materialization
constexpr Method kScalarMethods[] = {
    Method::kLet,    Method::kVar,   Method::kAssign, Method::kFnArg,    Method::kBuiltinArg,
    Method::kReturn, Method::kArray, Method::kStruct, Method::kBinaryOp,
};

/// Methods that support vector materialization
constexpr Method kVectorMethods[] = {
    Method::kLet,    Method::kVar,   Method::kAssign, Method::kFnArg,    Method::kBuiltinArg,
    Method::kReturn, Method::kArray, Method::kStruct, Method::kBinaryOp,
};

/// Methods that support matrix materialization
constexpr Method kMatrixMethods[] = {
    Method::kLet,    Method::kVar,   Method::kAssign, Method::kFnArg,
    Method::kReturn, Method::kArray, Method::kStruct, Method::kBinaryOp,
};

/// Methods that support array materialization
constexpr Method kArrayMethods[] = {
    Method::kLet,    Method::kVar,   Method::kAssign, Method::kFnArg,
    Method::kReturn, Method::kArray, Method::kStruct,
};

/// Methods that support materialization for switch cases
constexpr Method kSwitchMethods[] = {
    Method::kSwitchCond,
    Method::kSwitchCase,
    Method::kSwitchCondWithAbstractCase,
    Method::kSwitchCaseWithAbstractCase,
};

/// Methods that do not materialize
constexpr Method kNoMaterializeMethods[] = {
    Method::kPhonyAssign,  //
    Method::kBinaryOp,
    // TODO(crbug.com/tint/1504): Enable once "min" supports const evaluation
    // Method::kBuiltinArg,
};
INSTANTIATE_TEST_SUITE_P(
    MaterializeScalar,
    MaterializeAbstractNumericToConcreteType,
    testing::Combine(
        testing::Values(Expectation::kMaterialize),
        testing::ValuesIn(kScalarMethods),
        testing::ValuesIn(std::vector<Data>{
            Types<i32, AInt>(0_a, 0.0),                                                       //
            Types<i32, AInt>(1_a, 1.0),                                                       //
            Types<i32, AInt>(-1_a, -1.0),                                                     //
            Types<i32, AInt>(AInt(i32::Highest()), i32::Highest()),                           //
            Types<i32, AInt>(AInt(i32::Lowest()), i32::Lowest()),                             //
            Types<u32, AInt>(0_a, 0.0),                                                       //
            Types<u32, AInt>(1_a, 1.0),                                                       //
            Types<u32, AInt>(AInt(u32::Highest()), u32::Highest()),                           //
            Types<u32, AInt>(AInt(u32::Lowest()), u32::Lowest()),                             //
            Types<f32, AFloat>(0.0_a, 0.0),                                                   //
            Types<f32, AFloat>(AFloat(f32::Highest()), static_cast<double>(f32::Highest())),  //
            Types<f32, AFloat>(AFloat(f32::Lowest()), static_cast<double>(f32::Lowest())),    //
            Types<f32, AFloat>(AFloat(kPiF32), kPiF64),                                       //
            Types<f32, AFloat>(AFloat(kSubnormalF32), kSubnormalF32),                         //
            Types<f32, AFloat>(AFloat(-kSubnormalF32), -kSubnormalF32),                       //
            Types<f16, AFloat>(0.0_a, 0.0),                                                   //
            Types<f16, AFloat>(1.0_a, 1.0),                                                   //
            Types<f16, AFloat>(AFloat(f16::Highest()), static_cast<double>(f16::Highest())),  //
            Types<f16, AFloat>(AFloat(f16::Lowest()), static_cast<double>(f16::Lowest())),    //
            Types<f16, AFloat>(AFloat(kPiF16), kPiF64),                                       //
            Types<f16, AFloat>(AFloat(kSubnormalF16), kSubnormalF16),                         //
            Types<f16, AFloat>(AFloat(-kSubnormalF16), -kSubnormalF16),                       //
        })));

INSTANTIATE_TEST_SUITE_P(
    MaterializeVector,
    MaterializeAbstractNumericToConcreteType,
    testing::Combine(
        testing::Values(Expectation::kMaterialize),
        testing::ValuesIn(kVectorMethods),
        testing::ValuesIn(std::vector<Data>{
            Types<i32V, AIntV>(0_a, 0.0),                                                       //
            Types<i32V, AIntV>(1_a, 1.0),                                                       //
            Types<i32V, AIntV>(-1_a, -1.0),                                                     //
            Types<i32V, AIntV>(AInt(i32::Highest()), i32::Highest()),                           //
            Types<i32V, AIntV>(AInt(i32::Lowest()), i32::Lowest()),                             //
            Types<u32V, AIntV>(0_a, 0.0),                                                       //
            Types<u32V, AIntV>(1_a, 1.0),                                                       //
            Types<u32V, AIntV>(AInt(u32::Highest()), u32::Highest()),                           //
            Types<u32V, AIntV>(AInt(u32::Lowest()), u32::Lowest()),                             //
            Types<f32V, AFloatV>(0.0_a, 0.0),                                                   //
            Types<f32V, AFloatV>(1.0_a, 1.0),                                                   //
            Types<f32V, AFloatV>(-1.0_a, -1.0),                                                 //
            Types<f32V, AFloatV>(AFloat(f32::Highest()), static_cast<double>(f32::Highest())),  //
            Types<f32V, AFloatV>(AFloat(f32::Lowest()), static_cast<double>(f32::Lowest())),    //
            Types<f32V, AFloatV>(AFloat(kPiF32), kPiF64),                                       //
            Types<f32V, AFloatV>(AFloat(kSubnormalF32), kSubnormalF32),                         //
            Types<f32V, AFloatV>(AFloat(-kSubnormalF32), -kSubnormalF32),                       //
            Types<f16V, AFloatV>(0.0_a, 0.0),                                                   //
            Types<f16V, AFloatV>(1.0_a, 1.0),                                                   //
            Types<f16V, AFloatV>(-1.0_a, -1.0),                                                 //
            Types<f16V, AFloatV>(AFloat(f16::Highest()), static_cast<double>(f16::Highest())),  //
            Types<f16V, AFloatV>(AFloat(f16::Lowest()), static_cast<double>(f16::Lowest())),    //
            Types<f16V, AFloatV>(AFloat(kPiF16), kPiF64),                                       //
            Types<f16V, AFloatV>(AFloat(kSubnormalF16), kSubnormalF16),                         //
            Types<f16V, AFloatV>(AFloat(-kSubnormalF16), -kSubnormalF16),                       //
        })));

INSTANTIATE_TEST_SUITE_P(
    MaterializeVectorRuntimeIndex,
    MaterializeAbstractNumericToConcreteType,
    testing::Combine(
        testing::Values(Expectation::kMaterialize),
        testing::Values(Method::kRuntimeIndex),
        testing::ValuesIn(std::vector<Data>{
            Types<i32V, AIntV>(0_a, 0.0),                                                       //
            Types<i32V, AIntV>(1_a, 1.0),                                                       //
            Types<i32V, AIntV>(-1_a, -1.0),                                                     //
            Types<i32V, AIntV>(AInt(i32::Highest()), i32::Highest()),                           //
            Types<i32V, AIntV>(AInt(i32::Lowest()), i32::Lowest()),                             //
            Types<f32V, AFloatV>(0.0_a, 0.0),                                                   //
            Types<f32V, AFloatV>(1.0_a, 1.0),                                                   //
            Types<f32V, AFloatV>(-1.0_a, -1.0),                                                 //
            Types<f32V, AFloatV>(AFloat(f32::Highest()), static_cast<double>(f32::Highest())),  //
            Types<f32V, AFloatV>(AFloat(f32::Lowest()), static_cast<double>(f32::Lowest())),    //
            Types<f32V, AFloatV>(AFloat(kPiF32), kPiF64),                                       //
            Types<f32V, AFloatV>(AFloat(kSubnormalF32), kSubnormalF32),                         //
            Types<f32V, AFloatV>(AFloat(-kSubnormalF32), -kSubnormalF32),                       //
        })));

INSTANTIATE_TEST_SUITE_P(
    MaterializeMatrix,
    MaterializeAbstractNumericToConcreteType,
    testing::Combine(
        testing::Values(Expectation::kMaterialize),
        testing::ValuesIn(kMatrixMethods),
        testing::ValuesIn(std::vector<Data>{
            Types<f32M, AFloatM>(0.0_a, 0.0),                                                   //
            Types<f32M, AFloatM>(1.0_a, 1.0),                                                   //
            Types<f32M, AFloatM>(-1.0_a, -1.0),                                                 //
            Types<f32M, AFloatM>(AFloat(f32::Highest()), static_cast<double>(f32::Highest())),  //
            Types<f32M, AFloatM>(AFloat(f32::Lowest()), static_cast<double>(f32::Lowest())),    //
            Types<f32M, AFloatM>(AFloat(kPiF32), kPiF64),                                       //
            Types<f32M, AFloatM>(AFloat(kSubnormalF32), kSubnormalF32),                         //
            Types<f32M, AFloatM>(AFloat(-kSubnormalF32), -kSubnormalF32),                       //
            Types<f16M, AFloatM>(0.0_a, 0.0),                                                   //
            Types<f16M, AFloatM>(1.0_a, 1.0),                                                   //
            Types<f16M, AFloatM>(-1.0_a, -1.0),                                                 //
            Types<f16M, AFloatM>(AFloat(f16::Highest()), static_cast<double>(f16::Highest())),  //
            Types<f16M, AFloatM>(AFloat(f16::Lowest()), static_cast<double>(f16::Lowest())),    //
            Types<f16M, AFloatM>(AFloat(kPiF16), kPiF64),                                       //
            Types<f16M, AFloatM>(AFloat(kSubnormalF16), kSubnormalF16),                         //
            Types<f16M, AFloatM>(AFloat(-kSubnormalF16), -kSubnormalF16),                       //
        })));

INSTANTIATE_TEST_SUITE_P(
    MaterializeMatrixRuntimeIndex,
    MaterializeAbstractNumericToConcreteType,
    testing::Combine(
        testing::Values(Expectation::kMaterialize),
        testing::Values(Method::kRuntimeIndex),
        testing::ValuesIn(std::vector<Data>{
            Types<f32M, AFloatM>(0.0_a, 0.0),                                                   //
            Types<f32M, AFloatM>(1.0_a, 1.0),                                                   //
            Types<f32M, AFloatM>(-1.0_a, -1.0),                                                 //
            Types<f32M, AFloatM>(AFloat(f32::Highest()), static_cast<double>(f32::Highest())),  //
            Types<f32M, AFloatM>(AFloat(f32::Lowest()), static_cast<double>(f32::Lowest())),    //
            Types<f32M, AFloatM>(AFloat(kPiF32), kPiF64),                                       //
            Types<f32M, AFloatM>(AFloat(kSubnormalF32), kSubnormalF32),                         //
            Types<f32M, AFloatM>(AFloat(-kSubnormalF32), -kSubnormalF32),                       //
        })));

INSTANTIATE_TEST_SUITE_P(
    MaterializeSwitch,
    MaterializeAbstractNumericToConcreteType,
    testing::Combine(testing::Values(Expectation::kMaterialize),
                     testing::ValuesIn(kSwitchMethods),
                     testing::ValuesIn(std::vector<Data>{
                         Types<i32, AInt>(0_a, 0.0),                              //
                         Types<i32, AInt>(1_a, 1.0),                              //
                         Types<i32, AInt>(-1_a, -1.0),                            //
                         Types<i32, AInt>(AInt(i32::Highest()), i32::Highest()),  //
                         Types<i32, AInt>(AInt(i32::Lowest()), i32::Lowest()),    //
                         Types<u32, AInt>(0_a, 0.0),                              //
                         Types<u32, AInt>(1_a, 1.0),                              //
                         Types<u32, AInt>(AInt(u32::Highest()), u32::Highest()),  //
                         Types<u32, AInt>(AInt(u32::Lowest()), u32::Lowest()),    //
                     })));

INSTANTIATE_TEST_SUITE_P(
    MaterializeArray,
    MaterializeAbstractNumericToConcreteType,
    testing::Combine(
        testing::Values(Expectation::kMaterialize),
        testing::ValuesIn(kArrayMethods),
        testing::ValuesIn(std::vector<Data>{
            Types<i32A, AIntA>(0_a, 0.0),                                                       //
            Types<i32A, AIntA>(1_a, 1.0),                                                       //
            Types<i32A, AIntA>(-1_a, -1.0),                                                     //
            Types<i32A, AIntA>(AInt(i32::Highest()), i32::Highest()),                           //
            Types<i32A, AIntA>(AInt(i32::Lowest()), i32::Lowest()),                             //
            Types<u32A, AIntA>(0_a, 0.0),                                                       //
            Types<u32A, AIntA>(1_a, 1.0),                                                       //
            Types<u32A, AIntA>(AInt(u32::Highest()), u32::Highest()),                           //
            Types<u32A, AIntA>(AInt(u32::Lowest()), u32::Lowest()),                             //
            Types<f32A, AFloatA>(0.0_a, 0.0),                                                   //
            Types<f32A, AFloatA>(1.0_a, 1.0),                                                   //
            Types<f32A, AFloatA>(-1.0_a, -1.0),                                                 //
            Types<f32A, AFloatA>(AFloat(f32::Highest()), static_cast<double>(f32::Highest())),  //
            Types<f32A, AFloatA>(AFloat(f32::Lowest()), static_cast<double>(f32::Lowest())),    //
            Types<f32A, AFloatA>(AFloat(kPiF32), kPiF64),                                       //
            Types<f32A, AFloatA>(AFloat(kSubnormalF32), kSubnormalF32),                         //
            Types<f32A, AFloatA>(AFloat(-kSubnormalF32), -kSubnormalF32),                       //
            Types<f16A, AFloatA>(0.0_a, 0.0),                                                   //
            Types<f16A, AFloatA>(1.0_a, 1.0),                                                   //
            Types<f16A, AFloatA>(-1.0_a, -1.0),                                                 //
            Types<f16A, AFloatA>(AFloat(f16::Highest()), static_cast<double>(f16::Highest())),  //
            Types<f16A, AFloatA>(AFloat(f16::Lowest()), static_cast<double>(f16::Lowest())),    //
            Types<f16A, AFloatA>(AFloat(kPiF16), kPiF64),                                       //
            Types<f16A, AFloatA>(AFloat(kSubnormalF16), kSubnormalF16),                         //
            Types<f16A, AFloatA>(AFloat(-kSubnormalF16), -kSubnormalF16),                       //
        })));

INSTANTIATE_TEST_SUITE_P(
    MaterializeArrayRuntimeIndex,
    MaterializeAbstractNumericToConcreteType,
    testing::Combine(
        testing::Values(Expectation::kMaterialize),
        testing::Values(Method::kRuntimeIndex),
        testing::ValuesIn(std::vector<Data>{
            Types<f32A, AFloatA>(0.0_a, 0.0),                                                   //
            Types<f32A, AFloatA>(1.0_a, 1.0),                                                   //
            Types<f32A, AFloatA>(-1.0_a, -1.0),                                                 //
            Types<f32A, AFloatA>(AFloat(f32::Highest()), static_cast<double>(f32::Highest())),  //
            Types<f32A, AFloatA>(AFloat(f32::Lowest()), static_cast<double>(f32::Lowest())),    //
            Types<f32A, AFloatA>(AFloat(kPiF32), kPiF64),                                       //
            Types<f32A, AFloatA>(AFloat(kSubnormalF32), kSubnormalF32),                         //
            Types<f32A, AFloatA>(AFloat(-kSubnormalF32), -kSubnormalF32),                       //
        })));

INSTANTIATE_TEST_SUITE_P(MaterializeWorkgroupSize,
                         MaterializeAbstractNumericToConcreteType,
                         testing::Combine(testing::Values(Expectation::kMaterialize),
                                          testing::Values(Method::kWorkgroupSize),
                                          testing::ValuesIn(std::vector<Data>{
                                              Types<i32, AInt>(1_a, 1.0),          //
                                              Types<i32, AInt>(10_a, 10.0),        //
                                              Types<i32, AInt>(65535_a, 65535.0),  //
                                              Types<u32, AInt>(1_a, 1.0),          //
                                              Types<u32, AInt>(10_a, 10.0),        //
                                              Types<u32, AInt>(65535_a, 65535.0),  //
                                          })));

INSTANTIATE_TEST_SUITE_P(NoMaterialize,
                         MaterializeAbstractNumericToConcreteType,
                         testing::Combine(testing::Values(Expectation::kNoMaterialize),
                                          testing::ValuesIn(kNoMaterializeMethods),
                                          testing::ValuesIn(std::vector<Data>{
                                              Types<AInt, AInt>(1_a, 1_a),            //
                                              Types<AIntV, AIntV>(1_a, 1_a),          //
                                              Types<AFloat, AFloat>(1.0_a, 1.0_a),    //
                                              Types<AFloatV, AFloatV>(1.0_a, 1.0_a),  //
                                              Types<AFloatM, AFloatM>(1.0_a, 1.0_a),  //
                                          })));

INSTANTIATE_TEST_SUITE_P(InvalidConversion,
                         MaterializeAbstractNumericToConcreteType,
                         testing::Combine(testing::Values(Expectation::kInvalidConversion),
                                          testing::ValuesIn(kScalarMethods),
                                          testing::ValuesIn(std::vector<Data>{
                                              Types<i32, AFloat>(),    //
                                              Types<u32, AFloat>(),    //
                                              Types<i32V, AFloatV>(),  //
                                              Types<u32V, AFloatV>(),  //
                                              Types<i32A, AInt>(),     //
                                              Types<i32A, AIntV>(),    //
                                              Types<i32A, AFloat>(),   //
                                              Types<i32A, AFloatV>(),  //
                                          })));

INSTANTIATE_TEST_SUITE_P(
    ScalarValueCannotBeRepresented,
    MaterializeAbstractNumericToConcreteType,
    testing::Combine(testing::Values(Expectation::kValueCannotBeRepresented),
                     testing::ValuesIn(kScalarMethods),
                     testing::ValuesIn(std::vector<Data>{
                         Types<i32, AInt>(0_a, static_cast<double>(i32::kHighestValue) + 1),  //
                         Types<i32, AInt>(0_a, static_cast<double>(i32::kLowestValue) - 1),   //
                         Types<u32, AInt>(0_a, static_cast<double>(u32::kHighestValue) + 1),  //
                         Types<u32, AInt>(0_a, static_cast<double>(u32::kLowestValue) - 1),   //
                         Types<f32, AFloat>(0.0_a, kTooBigF32),                               //
                         Types<f32, AFloat>(0.0_a, -kTooBigF32),                              //
                         Types<f16, AFloat>(0.0_a, kTooBigF16),                               //
                         Types<f16, AFloat>(0.0_a, -kTooBigF16),                              //
                     })));

INSTANTIATE_TEST_SUITE_P(
    VectorValueCannotBeRepresented,
    MaterializeAbstractNumericToConcreteType,
    testing::Combine(testing::Values(Expectation::kValueCannotBeRepresented),
                     testing::ValuesIn(kVectorMethods),
                     testing::ValuesIn(std::vector<Data>{
                         Types<i32V, AIntV>(0_a, static_cast<double>(i32::kHighestValue) + 1),  //
                         Types<i32V, AIntV>(0_a, static_cast<double>(i32::kLowestValue) - 1),   //
                         Types<u32V, AIntV>(0_a, static_cast<double>(u32::kHighestValue) + 1),  //
                         Types<u32V, AIntV>(0_a, static_cast<double>(u32::kLowestValue) - 1),   //
                         Types<f32V, AFloatV>(0.0_a, kTooBigF32),                               //
                         Types<f32V, AFloatV>(0.0_a, -kTooBigF32),                              //
                         Types<f16V, AFloatV>(0.0_a, kTooBigF16),                               //
                         Types<f16V, AFloatV>(0.0_a, -kTooBigF16),                              //
                     })));

INSTANTIATE_TEST_SUITE_P(MatrixValueCannotBeRepresented,
                         MaterializeAbstractNumericToConcreteType,
                         testing::Combine(testing::Values(Expectation::kValueCannotBeRepresented),
                                          testing::ValuesIn(kMatrixMethods),
                                          testing::ValuesIn(std::vector<Data>{
                                              Types<f32M, AFloatM>(0.0_a, kTooBigF32),   //
                                              Types<f32M, AFloatM>(0.0_a, -kTooBigF32),  //
                                              Types<f16M, AFloatM>(0.0_a, kTooBigF16),   //
                                              Types<f16M, AFloatM>(0.0_a, -kTooBigF16),  //
                                          })));

}  // namespace materialize_abstract_numeric_to_concrete_type

////////////////////////////////////////////////////////////////////////////////////////////////////
// Tests that in the absence of a 'target type' an abstract-int will materialize to i32, and an
// abstract-float will materialize to f32.
////////////////////////////////////////////////////////////////////////////////////////////////////
namespace materialize_abstract_numeric_to_default_type {

// How should the materialization occur?
enum class Method {
    // var a = abstract_expr;
    kVar,

    // let a = abstract_expr;
    kLet,

    // min(abstract_expr, abstract_expr)
    kBuiltinArg,

    // bitcast<f32>(abstract_expr)
    kBitcastF32Arg,

    // bitcast<vec3<f32>>(abstract_expr)
    kBitcastVec3F32Arg,

    // array<i32, abstract_expr>()
    kArrayLength,

    // switch (abstract_expr) {
    //   case abstract_expr: {}
    //   default: {}
    // }
    kSwitch,

    // @workgroup_size(abstract_expr)
    // @compute
    // fn f() {}
    kWorkgroupSize,

    // arr[abstract_expr]
    kIndex,

    // abstract_expr[runtime-index]
    kRuntimeIndex,
};

static std::ostream& operator<<(std::ostream& o, Method m) {
    switch (m) {
        case Method::kVar:
            return o << "var";
        case Method::kLet:
            return o << "let";
        case Method::kBuiltinArg:
            return o << "builtin-arg";
        case Method::kBitcastF32Arg:
            return o << "bitcast-f32-arg";
        case Method::kBitcastVec3F32Arg:
            return o << "bitcast-vec3-f32-arg";
        case Method::kArrayLength:
            return o << "array-length";
        case Method::kSwitch:
            return o << "switch";
        case Method::kWorkgroupSize:
            return o << "workgroup-size";
        case Method::kIndex:
            return o << "index";
        case Method::kRuntimeIndex:
            return o << "runtime-index";
    }
    return o << "<unknown>";
}

struct Data {
    std::string expected_type_name;
    std::string expected_element_type_name;
    builder::sem_type_func_ptr expected_sem_ty;
    std::string abstract_type_name;
    builder::ast_expr_func_ptr abstract_expr;
    std::variant<AInt, AFloat> materialized_value;
    double literal_value;
};

template <typename EXPECTED_TYPE, typename ABSTRACT_TYPE, typename MATERIALIZED_TYPE>
Data Types(MATERIALIZED_TYPE materialized_value, double literal_value) {
    using ExpectedDataType = builder::DataType<EXPECTED_TYPE>;
    using AbstractDataType = builder::DataType<ABSTRACT_TYPE>;
    using TargetElementDataType = builder::DataType<typename ExpectedDataType::ElementType>;
    return {
        ExpectedDataType::Name(),       // expected_type_name
        TargetElementDataType::Name(),  // expected_element_type_name
        ExpectedDataType::Sem,          // expected_sem_ty
        AbstractDataType::Name(),       // abstract_type_name
        AbstractDataType::Expr,         // abstract_expr
        materialized_value,
        literal_value,
    };
}

static std::ostream& operator<<(std::ostream& o, const Data& c) {
    auto print_value = [&](auto&& v) { o << v; };
    o << "[" << c.expected_type_name << " <- " << c.abstract_type_name << "] [";
    std::visit(print_value, c.materialized_value);
    o << " <- " << c.literal_value << "]";
    return o;
}

using MaterializeAbstractNumericToDefaultType =
    MaterializeTest<std::tuple<Expectation, Method, Data>>;

TEST_P(MaterializeAbstractNumericToDefaultType, Test) {
    const auto& param = GetParam();
    const auto& expectation = std::get<0>(param);
    const auto& method = std::get<1>(param);
    const auto& data = std::get<2>(param);

    utils::Vector<const ast::Expression*, 4> abstract_exprs;
    auto abstract_expr = [&] {
        auto* expr = data.abstract_expr(*this, data.literal_value);
        abstract_exprs.Push(expr);
        return expr;
    };
    switch (method) {
        case Method::kVar:
            WrapInFunction(Decl(Var("a", abstract_expr())));
            break;
        case Method::kLet:
            WrapInFunction(Decl(Let("a", abstract_expr())));
            break;
        case Method::kBuiltinArg:
            WrapInFunction(CallStmt(Call("min", abstract_expr(), abstract_expr())));
            break;
        case Method::kBitcastF32Arg:
            WrapInFunction(Bitcast<f32>(abstract_expr()));
            break;
        case Method::kBitcastVec3F32Arg:
            WrapInFunction(Bitcast(ty.vec3<f32>(), abstract_expr()));
            break;
        case Method::kArrayLength:
            WrapInFunction(Construct(ty.array(ty.i32(), abstract_expr())));
            break;
        case Method::kSwitch:
            WrapInFunction(Switch(abstract_expr(),
                                  Case(abstract_expr()->As<ast::IntLiteralExpression>()),
                                  DefaultCase()));
            break;
        case Method::kWorkgroupSize:
            Func(
                "f", utils::Empty, ty.void_(), utils::Empty,
                utils::Vector{WorkgroupSize(abstract_expr()), Stage(ast::PipelineStage::kCompute)});
            break;
        case Method::kIndex:
            GlobalVar("arr", ty.array<i32, 4>(), ast::StorageClass::kPrivate);
            WrapInFunction(IndexAccessor("arr", abstract_expr()));
            break;
        case Method::kRuntimeIndex:
            auto* runtime_index = Var("runtime_index", Expr(1_i));
            WrapInFunction(runtime_index, IndexAccessor(abstract_expr(), runtime_index));
            break;
    }

    switch (expectation) {
        case Expectation::kMaterialize: {
            ASSERT_TRUE(r()->Resolve()) << r()->error();
            for (auto* expr : abstract_exprs) {
                auto* materialize = Sem().Get<sem::Materialize>(expr);
                ASSERT_NE(materialize, nullptr);
                CheckTypesAndValues(materialize, data.expected_sem_ty(*this),
                                    data.materialized_value);
            }
            break;
        }
        case Expectation::kInvalidConversion: {
            ASSERT_FALSE(r()->Resolve());
            std::string expect;
            switch (method) {
                case Method::kBuiltinArg:
                    expect = "error: no matching call to min(" + data.abstract_type_name + ", " +
                             data.abstract_type_name + ")";
                    break;
                default:
                    expect = "error: cannot convert value of type '" + data.abstract_type_name +
                             "' to type '" + data.expected_type_name + "'";
                    break;
            }
            EXPECT_THAT(r()->error(), testing::StartsWith(expect));
            break;
        }
        case Expectation::kValueCannotBeRepresented:
            ASSERT_FALSE(r()->Resolve());
            EXPECT_THAT(r()->error(), testing::HasSubstr("cannot be represented as '" +
                                                         data.expected_element_type_name + "'"));
            break;
        default:
            FAIL() << "unhandled expectation: " << expectation;
    }
}

/// Methods that support scalar materialization
constexpr Method kScalarMethods[] = {
    Method::kLet,
    Method::kVar,
    Method::kBuiltinArg,
    Method::kBitcastF32Arg,
};

/// Methods that support abstract-integer materialization
/// Note: Doesn't contain kWorkgroupSize or kArrayLength as they have tighter constraints on the
///       range of allowed integer values.
constexpr Method kAIntMethods[] = {
    Method::kSwitch,
    Method::kIndex,
};

/// Methods that support vector materialization
constexpr Method kVectorMethods[] = {
    Method::kLet,          Method::kVar, Method::kBuiltinArg, Method::kBitcastVec3F32Arg,
    Method::kRuntimeIndex,
};

/// Methods that support matrix materialization
constexpr Method kMatrixMethods[] = {
    Method::kLet,
    Method::kVar,
};

/// Methods that support array materialization
constexpr Method kArrayMethods[] = {
    Method::kLet,
    Method::kVar,
};

INSTANTIATE_TEST_SUITE_P(
    MaterializeScalar,
    MaterializeAbstractNumericToDefaultType,
    testing::Combine(
        testing::Values(Expectation::kMaterialize),
        testing::ValuesIn(kScalarMethods),
        testing::ValuesIn(std::vector<Data>{
            Types<i32, AInt>(0_a, 0.0),                                                       //
            Types<i32, AInt>(1_a, 1.0),                                                       //
            Types<i32, AInt>(-1_a, -1.0),                                                     //
            Types<i32, AInt>(AInt(i32::Highest()), i32::Highest()),                           //
            Types<i32, AInt>(AInt(i32::Lowest()), i32::Lowest()),                             //
            Types<f32, AFloat>(0.0_a, 0.0),                                                   //
            Types<f32, AFloat>(AFloat(f32::Highest()), static_cast<double>(f32::Highest())),  //
            Types<f32, AFloat>(AFloat(f32::Lowest()), static_cast<double>(f32::Lowest())),    //
            Types<f32, AFloat>(AFloat(kPiF32), kPiF64),                                       //
            Types<f32, AFloat>(AFloat(kSubnormalF32), kSubnormalF32),                         //
            Types<f32, AFloat>(AFloat(-kSubnormalF32), -kSubnormalF32),                       //
        })));

INSTANTIATE_TEST_SUITE_P(
    MaterializeVector,
    MaterializeAbstractNumericToDefaultType,
    testing::Combine(
        testing::Values(Expectation::kMaterialize),
        testing::ValuesIn(kVectorMethods),
        testing::ValuesIn(std::vector<Data>{
            Types<i32V, AIntV>(0_a, 0.0),                                                       //
            Types<i32V, AIntV>(1_a, 1.0),                                                       //
            Types<i32V, AIntV>(-1_a, -1.0),                                                     //
            Types<i32V, AIntV>(AInt(i32::Highest()), i32::Highest()),                           //
            Types<i32V, AIntV>(AInt(i32::Lowest()), i32::Lowest()),                             //
            Types<f32V, AFloatV>(0.0_a, 0.0),                                                   //
            Types<f32V, AFloatV>(1.0_a, 1.0),                                                   //
            Types<f32V, AFloatV>(-1.0_a, -1.0),                                                 //
            Types<f32V, AFloatV>(AFloat(f32::Highest()), static_cast<double>(f32::Highest())),  //
            Types<f32V, AFloatV>(AFloat(f32::Lowest()), static_cast<double>(f32::Lowest())),    //
            Types<f32V, AFloatV>(AFloat(kPiF32), kPiF64),                                       //
            Types<f32V, AFloatV>(AFloat(kSubnormalF32), kSubnormalF32),                         //
            Types<f32V, AFloatV>(AFloat(-kSubnormalF32), -kSubnormalF32),                       //
        })));

INSTANTIATE_TEST_SUITE_P(
    MaterializeMatrix,
    MaterializeAbstractNumericToDefaultType,
    testing::Combine(
        testing::Values(Expectation::kMaterialize),
        testing::ValuesIn(kMatrixMethods),
        testing::ValuesIn(std::vector<Data>{
            Types<f32M, AFloatM>(0.0_a, 0.0),                                                   //
            Types<f32M, AFloatM>(1.0_a, 1.0),                                                   //
            Types<f32M, AFloatM>(-1.0_a, -1.0),                                                 //
            Types<f32M, AFloatM>(AFloat(f32::Highest()), static_cast<double>(f32::Highest())),  //
            Types<f32M, AFloatM>(AFloat(f32::Lowest()), static_cast<double>(f32::Lowest())),    //
            Types<f32M, AFloatM>(AFloat(kPiF32), kPiF64),                                       //
            Types<f32M, AFloatM>(AFloat(kSubnormalF32), kSubnormalF32),                         //
            Types<f32M, AFloatM>(AFloat(-kSubnormalF32), -kSubnormalF32),                       //
        })));

INSTANTIATE_TEST_SUITE_P(
    MaterializeAInt,
    MaterializeAbstractNumericToDefaultType,
    testing::Combine(testing::Values(Expectation::kMaterialize),
                     testing::ValuesIn(kAIntMethods),
                     testing::ValuesIn(std::vector<Data>{
                         Types<i32, AInt>(0_a, 0.0),                              //
                         Types<i32, AInt>(10_a, 10.0),                            //
                         Types<i32, AInt>(AInt(i32::Highest()), i32::Highest()),  //
                         Types<i32, AInt>(AInt(i32::Lowest()), i32::Lowest()),    //
                     })));

INSTANTIATE_TEST_SUITE_P(
    MaterializeArray,
    MaterializeAbstractNumericToDefaultType,
    testing::Combine(
        testing::Values(Expectation::kMaterialize),
        testing::ValuesIn(kArrayMethods),
        testing::ValuesIn(std::vector<Data>{
            Types<i32A, AIntA>(0_a, 0.0),                                                       //
            Types<i32A, AIntA>(1_a, 1.0),                                                       //
            Types<i32A, AIntA>(-1_a, -1.0),                                                     //
            Types<i32A, AIntA>(AInt(i32::Highest()), i32::Highest()),                           //
            Types<i32A, AIntA>(AInt(i32::Lowest()), i32::Lowest()),                             //
            Types<f32A, AFloatA>(0.0_a, 0.0),                                                   //
            Types<f32A, AFloatA>(1.0_a, 1.0),                                                   //
            Types<f32A, AFloatA>(-1.0_a, -1.0),                                                 //
            Types<f32A, AFloatA>(AFloat(f32::Highest()), static_cast<double>(f32::Highest())),  //
            Types<f32A, AFloatA>(AFloat(f32::Lowest()), static_cast<double>(f32::Lowest())),    //
            Types<f32A, AFloatA>(AFloat(kPiF32), kPiF64),                                       //
            Types<f32A, AFloatA>(AFloat(kSubnormalF32), kSubnormalF32),                         //
            Types<f32A, AFloatA>(AFloat(-kSubnormalF32), -kSubnormalF32),                       //
        })));

INSTANTIATE_TEST_SUITE_P(
    MaterializeArrayLength,
    MaterializeAbstractNumericToDefaultType,
    testing::Combine(testing::Values(Expectation::kMaterialize),
                     testing::Values(Method::kArrayLength),
                     testing::ValuesIn(std::vector<Data>{
                         Types<i32, AInt>(1_a, 1.0),        //
                         Types<i32, AInt>(10_a, 10.0),      //
                         Types<i32, AInt>(1000_a, 1000.0),  //
                         // Note: i32::Highest() cannot be used due to max-byte-size validation
                     })));

INSTANTIATE_TEST_SUITE_P(MaterializeWorkgroupSize,
                         MaterializeAbstractNumericToDefaultType,
                         testing::Combine(testing::Values(Expectation::kMaterialize),
                                          testing::Values(Method::kWorkgroupSize),
                                          testing::ValuesIn(std::vector<Data>{
                                              Types<i32, AInt>(1_a, 1.0),          //
                                              Types<i32, AInt>(10_a, 10.0),        //
                                              Types<i32, AInt>(65535_a, 65535.0),  //
                                          })));

INSTANTIATE_TEST_SUITE_P(
    ScalarValueCannotBeRepresented,
    MaterializeAbstractNumericToDefaultType,
    testing::Combine(testing::Values(Expectation::kValueCannotBeRepresented),
                     testing::ValuesIn(kScalarMethods),
                     testing::ValuesIn(std::vector<Data>{
                         Types<i32, AInt>(0_a, static_cast<double>(i32::kHighestValue) + 1),  //
                         Types<i32, AInt>(0_a, static_cast<double>(i32::kLowestValue) - 1),   //
                         Types<f32, AFloat>(0.0_a, kTooBigF32),                               //
                         Types<f32, AFloat>(0.0_a, -kTooBigF32),                              //
                     })));

INSTANTIATE_TEST_SUITE_P(
    VectorValueCannotBeRepresented,
    MaterializeAbstractNumericToDefaultType,
    testing::Combine(testing::Values(Expectation::kValueCannotBeRepresented),
                     testing::ValuesIn(kVectorMethods),
                     testing::ValuesIn(std::vector<Data>{
                         Types<i32V, AIntV>(0_a, static_cast<double>(i32::kHighestValue) + 1),  //
                         Types<i32V, AIntV>(0_a, static_cast<double>(i32::kLowestValue) - 1),   //
                         Types<i32V, AIntV>(0_a, static_cast<double>(u32::kHighestValue) + 1),  //
                         Types<f32V, AFloatV>(0.0_a, kTooBigF32),                               //
                         Types<f32V, AFloatV>(0.0_a, -kTooBigF32),                              //
                     })));

INSTANTIATE_TEST_SUITE_P(MatrixValueCannotBeRepresented,
                         MaterializeAbstractNumericToDefaultType,
                         testing::Combine(testing::Values(Expectation::kValueCannotBeRepresented),
                                          testing::ValuesIn(kMatrixMethods),
                                          testing::ValuesIn(std::vector<Data>{
                                              Types<f32M, AFloatM>(0.0_a, kTooBigF32),   //
                                              Types<f32M, AFloatM>(0.0_a, -kTooBigF32),  //
                                          })));

INSTANTIATE_TEST_SUITE_P(
    AIntValueCannotBeRepresented,
    MaterializeAbstractNumericToDefaultType,
    testing::Combine(testing::Values(Expectation::kValueCannotBeRepresented),
                     testing::ValuesIn(kAIntMethods),
                     testing::ValuesIn(std::vector<Data>{
                         Types<i32, AInt>(0_a, static_cast<double>(i32::kHighestValue) + 1),  //
                         Types<i32, AInt>(0_a, static_cast<double>(i32::kLowestValue) - 1),   //
                     })));

INSTANTIATE_TEST_SUITE_P(
    WorkgroupSizeValueCannotBeRepresented,
    MaterializeAbstractNumericToDefaultType,
    testing::Combine(testing::Values(Expectation::kValueCannotBeRepresented),
                     testing::Values(Method::kWorkgroupSize),
                     testing::ValuesIn(std::vector<Data>{
                         Types<i32, AInt>(0_a, static_cast<double>(i32::kHighestValue) + 1),  //
                         Types<i32, AInt>(0_a, static_cast<double>(i32::kLowestValue) - 1),   //
                     })));

INSTANTIATE_TEST_SUITE_P(
    ArrayLengthValueCannotBeRepresented,
    MaterializeAbstractNumericToDefaultType,
    testing::Combine(testing::Values(Expectation::kValueCannotBeRepresented),
                     testing::Values(Method::kArrayLength),
                     testing::ValuesIn(std::vector<Data>{
                         Types<i32, AInt>(0_a, static_cast<double>(i32::kHighestValue) + 1),  //
                     })));

}  // namespace materialize_abstract_numeric_to_default_type

namespace materialize_abstract_numeric_to_unrelated_type {

using MaterializeAbstractNumericToUnrelatedType = resolver::ResolverTest;

TEST_F(MaterializeAbstractNumericToUnrelatedType, AIntToStructVarCtor) {
    Structure("S", utils::Vector{Member("a", ty.i32())});
    WrapInFunction(Decl(Var("v", ty.type_name("S"), Expr(Source{{12, 34}}, 1_a))));
    EXPECT_FALSE(r()->Resolve());
    EXPECT_THAT(
        r()->error(),
        testing::HasSubstr("error: cannot convert value of type 'abstract-int' to type 'S'"));
}

TEST_F(MaterializeAbstractNumericToUnrelatedType, AIntToStructLetCtor) {
    Structure("S", utils::Vector{Member("a", ty.i32())});
    WrapInFunction(Decl(Let("v", ty.type_name("S"), Expr(Source{{12, 34}}, 1_a))));
    EXPECT_FALSE(r()->Resolve());
    EXPECT_THAT(
        r()->error(),
        testing::HasSubstr("error: cannot convert value of type 'abstract-int' to type 'S'"));
}

}  // namespace materialize_abstract_numeric_to_unrelated_type

}  // namespace
}  // namespace tint::resolver
