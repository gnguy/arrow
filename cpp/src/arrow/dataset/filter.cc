// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/dataset/filter.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "arrow/buffer.h"
#include "arrow/buffer_builder.h"
#include "arrow/compute/context.h"
#include "arrow/compute/kernels/boolean.h"
#include "arrow/compute/kernels/cast.h"
#include "arrow/compute/kernels/compare.h"
#include "arrow/compute/kernels/filter.h"
#include "arrow/compute/kernels/isin.h"
#include "arrow/dataset/dataset.h"
#include "arrow/record_batch.h"
#include "arrow/result.h"
#include "arrow/scalar.h"
#include "arrow/type_fwd.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/iterator.h"
#include "arrow/util/logging.h"
#include "arrow/visitor_inline.h"

namespace arrow {
namespace dataset {

using arrow::compute::Datum;
using internal::checked_cast;
using internal::checked_pointer_cast;

inline std::shared_ptr<ScalarExpression> NullExpression() {
  return std::make_shared<ScalarExpression>(std::make_shared<BooleanScalar>());
}

inline Datum NullDatum() { return Datum(std::make_shared<NullScalar>()); }

bool IsNullDatum(const Datum& datum) {
  if (datum.is_scalar()) {
    auto scalar = datum.scalar();
    return !scalar->is_valid;
  }

  auto array_data = datum.array();
  return array_data->GetNullCount() == array_data->length;
}

struct Comparison {
  enum type {
    LESS,
    EQUAL,
    GREATER,
    NULL_,
  };
};

struct CompareVisitor {
  template <typename T>
  using ScalarType = typename TypeTraits<T>::ScalarType;

  Status Visit(const NullType&) {
    result_ = Comparison::NULL_;
    return Status::OK();
  }

  Status Visit(const BooleanType&) { return CompareValues<BooleanType>(); }

  template <typename T>
  enable_if_number<T, Status> Visit(const T&) {
    return CompareValues<T>();
  }

  template <typename T>
  enable_if_binary_like<T, Status> Visit(const T&) {
    auto lhs = checked_cast<const ScalarType<T>&>(lhs_).value;
    auto rhs = checked_cast<const ScalarType<T>&>(rhs_).value;
    auto cmp = std::memcmp(lhs->data(), rhs->data(), std::min(lhs->size(), rhs->size()));
    if (cmp == 0) {
      return CompareValues(lhs->size(), rhs->size());
    }
    return CompareValues(cmp, 0);
  }

  Status Visit(const Decimal128Type&) { return CompareValues<Decimal128Type>(); }

  // explicit because both integral and floating point conditions match half float
  Status Visit(const HalfFloatType&) {
    // TODO(bkietz) whenever we vendor a float16, this can be implemented
    return Status::NotImplemented("comparison of scalars of type ", *lhs_.type);
  }

  Status Visit(const DataType&) {
    return Status::NotImplemented("comparison of scalars of type ", *lhs_.type);
  }

  // defer comparison to ScalarType<T>::value
  template <typename T>
  Status CompareValues() {
    auto lhs = checked_cast<const ScalarType<T>&>(lhs_).value;
    auto rhs = checked_cast<const ScalarType<T>&>(rhs_).value;
    return CompareValues(lhs, rhs);
  }

  // defer comparison to explicit values
  template <typename Value>
  Status CompareValues(Value lhs, Value rhs) {
    result_ = lhs < rhs ? Comparison::LESS
                        : lhs == rhs ? Comparison::EQUAL : Comparison::GREATER;
    return Status::OK();
  }

  Comparison::type result_;
  const Scalar& lhs_;
  const Scalar& rhs_;
};

// Compare two scalars
// if either is null, return is null
// TODO(bkietz) extract this to scalar.h
Result<Comparison::type> Compare(const Scalar& lhs, const Scalar& rhs) {
  if (!lhs.type->Equals(*rhs.type)) {
    return Status::TypeError("Cannot compare scalars of differing type: ", *lhs.type,
                             " vs ", *rhs.type);
  }
  if (!lhs.is_valid || !rhs.is_valid) {
    return Comparison::NULL_;
  }
  CompareVisitor vis{Comparison::NULL_, lhs, rhs};
  RETURN_NOT_OK(VisitTypeInline(*lhs.type, &vis));
  return vis.result_;
}

compute::CompareOperator InvertCompareOperator(compute::CompareOperator op) {
  using compute::CompareOperator;

  switch (op) {
    case CompareOperator::EQUAL:
      return CompareOperator::NOT_EQUAL;

    case CompareOperator::NOT_EQUAL:
      return CompareOperator::EQUAL;

    case CompareOperator::GREATER:
      return CompareOperator::LESS_EQUAL;

    case CompareOperator::GREATER_EQUAL:
      return CompareOperator::LESS;

    case CompareOperator::LESS:
      return CompareOperator::GREATER_EQUAL;

    case CompareOperator::LESS_EQUAL:
      return CompareOperator::GREATER;

    default:
      break;
  }

  DCHECK(false);
  return CompareOperator::EQUAL;
}

template <typename Boolean>
std::shared_ptr<Expression> InvertBoolean(const Boolean& expr) {
  auto lhs = Invert(*expr.left_operand());
  auto rhs = Invert(*expr.right_operand());

  if (std::is_same<Boolean, AndExpression>::value) {
    return std::make_shared<OrExpression>(std::move(lhs), std::move(rhs));
  }

  if (std::is_same<Boolean, OrExpression>::value) {
    return std::make_shared<AndExpression>(std::move(lhs), std::move(rhs));
  }

  return nullptr;
}

std::shared_ptr<Expression> Invert(const Expression& expr) {
  switch (expr.type()) {
    case ExpressionType::NOT:
      return checked_cast<const NotExpression&>(expr).operand();

    case ExpressionType::AND:
      return InvertBoolean(checked_cast<const AndExpression&>(expr));

    case ExpressionType::OR:
      return InvertBoolean(checked_cast<const OrExpression&>(expr));

    case ExpressionType::COMPARISON: {
      const auto& comparison = checked_cast<const ComparisonExpression&>(expr);
      auto inverted_op = InvertCompareOperator(comparison.op());
      return std::make_shared<ComparisonExpression>(
          inverted_op, comparison.left_operand(), comparison.right_operand());
    }

    default:
      break;
  }
  return nullptr;
}

std::shared_ptr<Expression> Expression::Assume(const Expression& given) const {
  if (given.type() == ExpressionType::COMPARISON) {
    const auto& given_cmp = checked_cast<const ComparisonExpression&>(given);
    if (given_cmp.op() == compute::CompareOperator::EQUAL) {
      if (this->Equals(given_cmp.left_operand()) &&
          given_cmp.right_operand()->type() == ExpressionType::SCALAR) {
        return given_cmp.right_operand();
      }

      if (this->Equals(given_cmp.right_operand()) &&
          given_cmp.left_operand()->type() == ExpressionType::SCALAR) {
        return given_cmp.left_operand();
      }
    }
  }

  return Copy();
}

std::shared_ptr<Expression> ComparisonExpression::Assume(const Expression& given) const {
  switch (given.type()) {
    case ExpressionType::COMPARISON: {
      return AssumeGivenComparison(checked_cast<const ComparisonExpression&>(given));
    }

    case ExpressionType::NOT: {
      const auto& given_not = checked_cast<const NotExpression&>(given);
      if (auto inverted = Invert(*given_not.operand())) {
        return Assume(*inverted);
      }
      return Copy();
    }

    case ExpressionType::OR: {
      const auto& given_or = checked_cast<const OrExpression&>(given);

      auto left_simplified = Assume(*given_or.left_operand());
      auto right_simplified = Assume(*given_or.right_operand());

      // The result of simplification against the operands of an OrExpression
      // cannot be used unless they are identical
      if (left_simplified->Equals(right_simplified)) {
        return left_simplified;
      }

      return Copy();
    }

    case ExpressionType::AND: {
      const auto& given_and = checked_cast<const AndExpression&>(given);

      auto simplified = Copy();
      simplified = simplified->Assume(*given_and.left_operand());
      simplified = simplified->Assume(*given_and.right_operand());
      return simplified;
    }

      // TODO(bkietz) we should be able to use ExpressionType::IN here

    default:
      break;
  }

  return Copy();
}

// Try to simplify one comparison against another comparison.
// For example,
// ("x"_ > 3) is a subset of ("x"_ > 2), so ("x"_ > 2).Assume("x"_ > 3) == (true)
// ("x"_ < 0) is disjoint with ("x"_ > 2), so ("x"_ > 2).Assume("x"_ < 0) == (false)
// If simplification to (true) or (false) is not possible, pass e through unchanged.
std::shared_ptr<Expression> ComparisonExpression::AssumeGivenComparison(
    const ComparisonExpression& given) const {
  if (!left_operand_->Equals(given.left_operand_)) {
    return Copy();
  }

  for (auto rhs : {right_operand_, given.right_operand_}) {
    if (rhs->type() != ExpressionType::SCALAR) {
      return Copy();
    }
  }

  const auto& this_rhs = checked_cast<const ScalarExpression&>(*right_operand_).value();
  const auto& given_rhs =
      checked_cast<const ScalarExpression&>(*given.right_operand_).value();

  auto cmp = Compare(*this_rhs, *given_rhs).ValueOrDie();

  if (cmp == Comparison::NULL_) {
    // the RHS of e or given was null
    return NullExpression();
  }

  static auto always = scalar(true);
  static auto never = scalar(false);

  using compute::CompareOperator;

  if (cmp == Comparison::GREATER) {
    // the rhs of e is greater than that of given
    switch (op()) {
      case CompareOperator::EQUAL:
      case CompareOperator::GREATER:
      case CompareOperator::GREATER_EQUAL:
        switch (given.op()) {
          case CompareOperator::EQUAL:
          case CompareOperator::LESS:
          case CompareOperator::LESS_EQUAL:
            return never;
          default:
            return Copy();
        }
      case CompareOperator::NOT_EQUAL:
      case CompareOperator::LESS:
      case CompareOperator::LESS_EQUAL:
        switch (given.op()) {
          case CompareOperator::EQUAL:
          case CompareOperator::LESS:
          case CompareOperator::LESS_EQUAL:
            return always;
          default:
            return Copy();
        }
      default:
        return Copy();
    }
  }

  if (cmp == Comparison::LESS) {
    // the rhs of e is less than that of given
    switch (op()) {
      case CompareOperator::EQUAL:
      case CompareOperator::LESS:
      case CompareOperator::LESS_EQUAL:
        switch (given.op()) {
          case CompareOperator::EQUAL:
          case CompareOperator::GREATER:
          case CompareOperator::GREATER_EQUAL:
            return never;
          default:
            return Copy();
        }
      case CompareOperator::NOT_EQUAL:
      case CompareOperator::GREATER:
      case CompareOperator::GREATER_EQUAL:
        switch (given.op()) {
          case CompareOperator::EQUAL:
          case CompareOperator::GREATER:
          case CompareOperator::GREATER_EQUAL:
            return always;
          default:
            return Copy();
        }
      default:
        return Copy();
    }
  }

  DCHECK_EQ(cmp, Comparison::EQUAL);

  // the rhs of the comparisons are equal
  switch (op_) {
    case CompareOperator::EQUAL:
      switch (given.op()) {
        case CompareOperator::NOT_EQUAL:
        case CompareOperator::GREATER:
        case CompareOperator::LESS:
          return never;
        case CompareOperator::EQUAL:
          return always;
        default:
          return Copy();
      }
    case CompareOperator::NOT_EQUAL:
      switch (given.op()) {
        case CompareOperator::EQUAL:
          return never;
        case CompareOperator::NOT_EQUAL:
        case CompareOperator::GREATER:
        case CompareOperator::LESS:
          return always;
        default:
          return Copy();
      }
    case CompareOperator::GREATER:
      switch (given.op()) {
        case CompareOperator::EQUAL:
        case CompareOperator::LESS_EQUAL:
        case CompareOperator::LESS:
          return never;
        case CompareOperator::GREATER:
          return always;
        default:
          return Copy();
      }
    case CompareOperator::GREATER_EQUAL:
      switch (given.op()) {
        case CompareOperator::LESS:
          return never;
        case CompareOperator::EQUAL:
        case CompareOperator::GREATER:
        case CompareOperator::GREATER_EQUAL:
          return always;
        default:
          return Copy();
      }
    case CompareOperator::LESS:
      switch (given.op()) {
        case CompareOperator::EQUAL:
        case CompareOperator::GREATER:
        case CompareOperator::GREATER_EQUAL:
          return never;
        case CompareOperator::LESS:
          return always;
        default:
          return Copy();
      }
    case CompareOperator::LESS_EQUAL:
      switch (given.op()) {
        case CompareOperator::GREATER:
          return never;
        case CompareOperator::EQUAL:
        case CompareOperator::LESS:
        case CompareOperator::LESS_EQUAL:
          return always;
        default:
          return Copy();
      }
    default:
      return Copy();
  }
  return Copy();
}

std::shared_ptr<Expression> AndExpression::Assume(const Expression& given) const {
  auto left_operand = left_operand_->Assume(given);
  auto right_operand = right_operand_->Assume(given);

  // if either of the operands is trivially false then so is this AND
  if (left_operand->Equals(false) || right_operand->Equals(false)) {
    return scalar(false);
  }

  // if either operand is trivially null then so is this AND
  if (left_operand->IsNull() || right_operand->IsNull()) {
    return NullExpression();
  }

  // if one of the operands is trivially true then drop it
  if (left_operand->Equals(true)) {
    return right_operand;
  }
  if (right_operand->Equals(true)) {
    return left_operand;
  }

  // if neither of the operands is trivial, simply construct a new AND
  return and_(std::move(left_operand), std::move(right_operand));
}

std::shared_ptr<Expression> OrExpression::Assume(const Expression& given) const {
  auto left_operand = left_operand_->Assume(given);
  auto right_operand = right_operand_->Assume(given);

  // if either of the operands is trivially true then so is this OR
  if (left_operand->Equals(true) || right_operand->Equals(true)) {
    return scalar(true);
  }

  // if either operand is trivially null then so is this OR
  if (left_operand->IsNull() || right_operand->IsNull()) {
    return NullExpression();
  }

  // if one of the operands is trivially false then drop it
  if (left_operand->Equals(false)) {
    return right_operand;
  }
  if (right_operand->Equals(false)) {
    return left_operand;
  }

  // if neither of the operands is trivial, simply construct a new OR
  return or_(std::move(left_operand), std::move(right_operand));
}

std::shared_ptr<Expression> NotExpression::Assume(const Expression& given) const {
  auto operand = operand_->Assume(given);

  if (operand->IsNull()) {
    return NullExpression();
  }
  if (operand->Equals(true)) {
    return scalar(false);
  }
  if (operand->Equals(false)) {
    return scalar(true);
  }

  return Copy();
}

std::shared_ptr<Expression> InExpression::Assume(const Expression& given) const {
  auto operand = operand_->Assume(given);
  if (operand->type() != ExpressionType::SCALAR) {
    return std::make_shared<InExpression>(std::move(operand), set_);
  }

  if (operand->IsNull()) {
    return scalar(set_->null_count() > 0);
  }

  const auto& value = checked_cast<const ScalarExpression&>(*operand).value();

  Datum out;
  compute::FunctionContext ctx;
  arrow::compute::CompareOptions eq(compute::CompareOperator::EQUAL);
  if (!compute::Compare(&ctx, Datum(set_), Datum(value), eq, &out).ok()) {
    return std::make_shared<InExpression>(std::move(operand), set_);
  }

  DCHECK(out.is_array());
  DCHECK_EQ(out.type()->id(), Type::BOOL);
  auto out_array = checked_pointer_cast<BooleanArray>(out.make_array());

  for (int64_t i = 0; i < out_array->length(); ++i) {
    if (out_array->IsValid(i) && out_array->Value(i)) {
      return scalar(true);
    }
  }
  return scalar(false);
}

std::shared_ptr<Expression> IsValidExpression::Assume(const Expression& given) const {
  auto operand = operand_->Assume(given);
  if (operand->type() == ExpressionType::SCALAR) {
    return scalar(!operand->IsNull());
  }

  return std::make_shared<IsValidExpression>(std::move(operand));
}

std::shared_ptr<Expression> CastExpression::Assume(const Expression& given) const {
  auto operand = operand_->Assume(given);
  if (arrow::util::holds_alternative<std::shared_ptr<DataType>>(to_)) {
    auto to_type = arrow::util::get<std::shared_ptr<DataType>>(to_);
    return std::make_shared<CastExpression>(std::move(operand), std::move(to_type),
                                            options_);
  }
  auto like = arrow::util::get<std::shared_ptr<Expression>>(to_)->Assume(given);
  return std::make_shared<CastExpression>(std::move(operand), std::move(like), options_);
}

std::string FieldExpression::ToString() const {
  return std::string("field(") + name_ + ")";
}

std::string OperatorName(compute::CompareOperator op) {
  using compute::CompareOperator;
  switch (op) {
    case CompareOperator::EQUAL:
      return "EQUAL";
    case CompareOperator::NOT_EQUAL:
      return "NOT_EQUAL";
    case CompareOperator::LESS:
      return "LESS";
    case CompareOperator::LESS_EQUAL:
      return "LESS_EQUAL";
    case CompareOperator::GREATER:
      return "GREATER";
    case CompareOperator::GREATER_EQUAL:
      return "GREATER_EQUAL";
    default:
      DCHECK(false);
  }
  return "";
}

std::string ScalarExpression::ToString() const {
  if (!value_->is_valid) {
    return "scalar<" + value_->type->ToString() + ", null>()";
  }

  return "scalar<" + value_->type->ToString() + ">(" + value_->ToString() + ")";
}

static std::string EulerNotation(std::string fn, const ExpressionVector& operands) {
  fn += "(";
  bool comma = false;
  for (const auto& operand : operands) {
    if (comma) {
      fn += ", ";
    } else {
      comma = true;
    }
    fn += operand->ToString();
  }
  fn += ")";
  return fn;
}

std::string AndExpression::ToString() const {
  return EulerNotation("AND", {left_operand_, right_operand_});
}

std::string OrExpression::ToString() const {
  return EulerNotation("OR", {left_operand_, right_operand_});
}

std::string NotExpression::ToString() const { return EulerNotation("NOT", {operand_}); }

std::string IsValidExpression::ToString() const {
  return EulerNotation("IS_VALID", {operand_});
}

std::string InExpression::ToString() const {
  return EulerNotation("IN<" + set_->ToString() + ">", {operand_});
}

std::string CastExpression::ToString() const {
  std::string name;
  if (arrow::util::holds_alternative<std::shared_ptr<DataType>>(to_)) {
    auto to_type = arrow::util::get<std::shared_ptr<DataType>>(to_);
    name = "CAST<" + to_type->ToString() + ">";
  } else {
    auto like = arrow::util::get<std::shared_ptr<Expression>>(to_);
    name = "CAST<TYPEOF(" + like->ToString() + ")>";
  }
  return EulerNotation(name, {operand_});
}

std::string ComparisonExpression::ToString() const {
  return EulerNotation(OperatorName(op()), {left_operand_, right_operand_});
}

bool UnaryExpression::Equals(const Expression& other) const {
  return type_ == other.type() &&
         operand_->Equals(checked_cast<const UnaryExpression&>(other).operand_);
}

bool BinaryExpression::Equals(const Expression& other) const {
  return type_ == other.type() &&
         left_operand_->Equals(
             checked_cast<const BinaryExpression&>(other).left_operand_) &&
         right_operand_->Equals(
             checked_cast<const BinaryExpression&>(other).right_operand_);
}

bool ComparisonExpression::Equals(const Expression& other) const {
  return BinaryExpression::Equals(other) &&
         op_ == checked_cast<const ComparisonExpression&>(other).op_;
}

bool ScalarExpression::Equals(const Expression& other) const {
  return other.type() == ExpressionType::SCALAR &&
         value_->Equals(checked_cast<const ScalarExpression&>(other).value_);
}

bool FieldExpression::Equals(const Expression& other) const {
  return other.type() == ExpressionType::FIELD &&
         name_ == checked_cast<const FieldExpression&>(other).name_;
}

bool Expression::Equals(const std::shared_ptr<Expression>& other) const {
  if (other == NULLPTR) {
    return false;
  }
  return Equals(*other);
}

bool Expression::IsNull() const {
  if (type_ != ExpressionType::SCALAR) {
    return false;
  }

  const auto& scalar = checked_cast<const ScalarExpression&>(*this).value();
  if (!scalar->is_valid) {
    return true;
  }

  return false;
}

InExpression Expression::In(std::shared_ptr<Array> set) const {
  return InExpression(Copy(), std::move(set));
}

IsValidExpression Expression::IsValid() const { return IsValidExpression(Copy()); }

std::shared_ptr<Expression> FieldExpression::Copy() const {
  return std::make_shared<FieldExpression>(*this);
}

std::shared_ptr<Expression> ScalarExpression::Copy() const {
  return std::make_shared<ScalarExpression>(*this);
}

std::shared_ptr<AndExpression> and_(std::shared_ptr<Expression> lhs,
                                    std::shared_ptr<Expression> rhs) {
  return std::make_shared<AndExpression>(std::move(lhs), std::move(rhs));
}

std::shared_ptr<Expression> and_(const ExpressionVector& subexpressions) {
  if (subexpressions.size() == 0) {
    return scalar(true);
  }
  return std::accumulate(
      subexpressions.begin(), subexpressions.end(), std::shared_ptr<Expression>(),
      [](std::shared_ptr<Expression> acc, const std::shared_ptr<Expression>& next) {
        return acc == nullptr ? next : and_(std::move(acc), next);
      });
}

std::shared_ptr<OrExpression> or_(std::shared_ptr<Expression> lhs,
                                  std::shared_ptr<Expression> rhs) {
  return std::make_shared<OrExpression>(std::move(lhs), std::move(rhs));
}

std::shared_ptr<Expression> or_(const ExpressionVector& subexpressions) {
  if (subexpressions.size() == 0) {
    return scalar(false);
  }
  return std::accumulate(
      subexpressions.begin(), subexpressions.end(), std::shared_ptr<Expression>(),
      [](std::shared_ptr<Expression> acc, const std::shared_ptr<Expression>& next) {
        return acc == nullptr ? next : or_(std::move(acc), next);
      });
}

std::shared_ptr<NotExpression> not_(std::shared_ptr<Expression> operand) {
  return std::make_shared<NotExpression>(std::move(operand));
}

AndExpression operator&&(const Expression& lhs, const Expression& rhs) {
  return AndExpression(lhs.Copy(), rhs.Copy());
}

OrExpression operator||(const Expression& lhs, const Expression& rhs) {
  return OrExpression(lhs.Copy(), rhs.Copy());
}

NotExpression operator!(const Expression& rhs) { return NotExpression(rhs.Copy()); }

CastExpression Expression::CastTo(std::shared_ptr<DataType> type,
                                  compute::CastOptions options) const {
  return CastExpression(Copy(), type, std::move(options));
}

CastExpression Expression::CastLike(std::shared_ptr<Expression> expr,
                                    compute::CastOptions options) const {
  return CastExpression(Copy(), std::move(expr), std::move(options));
}

CastExpression Expression::CastLike(const Expression& expr,
                                    compute::CastOptions options) const {
  return CastLike(expr.Copy(), std::move(options));
}

Result<std::shared_ptr<DataType>> ComparisonExpression::Validate(
    const Schema& schema) const {
  ARROW_ASSIGN_OR_RAISE(auto lhs_type, left_operand_->Validate(schema));
  ARROW_ASSIGN_OR_RAISE(auto rhs_type, right_operand_->Validate(schema));

  if (lhs_type->id() == Type::NA || rhs_type->id() == Type::NA) {
    return boolean();
  }

  if (!lhs_type->Equals(rhs_type)) {
    return Status::TypeError("cannot compare expressions of differing type, ", *lhs_type,
                             " vs ", *rhs_type);
  }

  return boolean();
}

Status EnsureNullOrBool(const std::string& msg_prefix,
                        const std::shared_ptr<DataType>& type) {
  if (type->id() == Type::BOOL || type->id() == Type::NA) {
    return Status::OK();
  }
  return Status::TypeError(msg_prefix, *type);
}

Result<std::shared_ptr<DataType>> ValidateBoolean(const ExpressionVector& operands,
                                                  const Schema& schema) {
  for (const auto& operand : operands) {
    ARROW_ASSIGN_OR_RAISE(auto type, operand->Validate(schema));
    RETURN_NOT_OK(
        EnsureNullOrBool("cannot combine expressions including one of type ", type));
  }
  return boolean();
}

Result<std::shared_ptr<DataType>> AndExpression::Validate(const Schema& schema) const {
  return ValidateBoolean({left_operand_, right_operand_}, schema);
}

Result<std::shared_ptr<DataType>> OrExpression::Validate(const Schema& schema) const {
  return ValidateBoolean({left_operand_, right_operand_}, schema);
}

Result<std::shared_ptr<DataType>> NotExpression::Validate(const Schema& schema) const {
  return ValidateBoolean({operand_}, schema);
}

Result<std::shared_ptr<DataType>> InExpression::Validate(const Schema& schema) const {
  ARROW_ASSIGN_OR_RAISE(auto operand_type, operand_->Validate(schema));
  if (!operand_type->Equals(set_->type())) {
    return Status::TypeError("mismatch: set type ", *set_->type(), " vs operand type ",
                             *operand_type);
  }
  // TODO(bkietz) check if IsIn supports operand_type
  return boolean();
}

Result<std::shared_ptr<DataType>> IsValidExpression::Validate(
    const Schema& schema) const {
  ARROW_ASSIGN_OR_RAISE(std::ignore, operand_->Validate(schema));
  return boolean();
}

Result<std::shared_ptr<DataType>> CastExpression::Validate(const Schema& schema) const {
  ARROW_ASSIGN_OR_RAISE(auto operand_type, operand_->Validate(schema));
  std::shared_ptr<DataType> to_type;
  if (arrow::util::holds_alternative<std::shared_ptr<DataType>>(to_)) {
    to_type = arrow::util::get<std::shared_ptr<DataType>>(to_);
  } else {
    auto like = arrow::util::get<std::shared_ptr<Expression>>(to_);
    ARROW_ASSIGN_OR_RAISE(to_type, like->Validate(schema));
  }

  std::unique_ptr<compute::UnaryKernel> kernel;
  RETURN_NOT_OK(GetCastFunction(*operand_type, to_type, options_, &kernel));
  return to_type;
}

Result<std::shared_ptr<DataType>> ScalarExpression::Validate(const Schema& schema) const {
  return value_->type;
}

Result<std::shared_ptr<DataType>> FieldExpression::Validate(const Schema& schema) const {
  if (auto field = schema.GetFieldByName(name_)) {
    return field->type();
  }
  return null();
}

struct InsertImplicitCastsImpl {
  struct Validated {
    std::shared_ptr<Expression> expr;
    std::shared_ptr<DataType> type;
  };

  Result<Validated> Validate(const Expression& expr) {
    Validated out;
    ARROW_ASSIGN_OR_RAISE(out.expr, InsertImplicitCasts(expr, schema_));
    ARROW_ASSIGN_OR_RAISE(out.type, out.expr->Validate(schema_));
    return std::move(out);
  }

  Result<std::shared_ptr<Expression>> operator()(const NotExpression& expr) {
    ARROW_ASSIGN_OR_RAISE(auto op, Validate(*expr.operand()));

    if (op.type->id() != Type::BOOL) {
      op.expr = op.expr->CastTo(boolean()).Copy();
    }
    return not_(std::move(op.expr));
  }

  Result<std::shared_ptr<Expression>> operator()(const AndExpression& expr) {
    ARROW_ASSIGN_OR_RAISE(auto lhs, Validate(*expr.left_operand()));
    ARROW_ASSIGN_OR_RAISE(auto rhs, Validate(*expr.right_operand()));

    if (lhs.type->id() != Type::BOOL) {
      lhs.expr = lhs.expr->CastTo(boolean()).Copy();
    }
    if (rhs.type->id() != Type::BOOL) {
      rhs.expr = rhs.expr->CastTo(boolean()).Copy();
    }
    return and_(std::move(lhs.expr), std::move(rhs.expr));
  }

  Result<std::shared_ptr<Expression>> operator()(const OrExpression& expr) {
    ARROW_ASSIGN_OR_RAISE(auto lhs, Validate(*expr.left_operand()));
    ARROW_ASSIGN_OR_RAISE(auto rhs, Validate(*expr.right_operand()));

    if (lhs.type->id() != Type::BOOL) {
      lhs.expr = lhs.expr->CastTo(boolean()).Copy();
    }
    if (rhs.type->id() != Type::BOOL) {
      rhs.expr = rhs.expr->CastTo(boolean()).Copy();
    }
    return or_(std::move(lhs.expr), std::move(rhs.expr));
  }

  Result<std::shared_ptr<Expression>> operator()(const ComparisonExpression& expr) {
    ARROW_ASSIGN_OR_RAISE(auto lhs, Validate(*expr.left_operand()));
    ARROW_ASSIGN_OR_RAISE(auto rhs, Validate(*expr.right_operand()));

    if (lhs.type->Equals(rhs.type)) {
      return expr.Copy();
    }

    if (lhs.expr->type() == ExpressionType::SCALAR) {
      lhs.expr = lhs.expr->CastTo(rhs.type).Copy();
    } else {
      rhs.expr = rhs.expr->CastTo(lhs.type).Copy();
    }
    return std::make_shared<ComparisonExpression>(expr.op(), std::move(lhs.expr),
                                                  std::move(rhs.expr));
  }

  Result<std::shared_ptr<Expression>> operator()(const Expression& expr) const {
    return expr.Copy();
  }

  const Schema& schema_;
};

Result<std::shared_ptr<Expression>> InsertImplicitCasts(const Expression& expr,
                                                        const Schema& schema) {
  return VisitExpression(expr, InsertImplicitCastsImpl{schema});
}

std::vector<std::string> FieldsInExpression(const Expression& expr) {
  struct {
    void operator()(const FieldExpression& expr) { fields.push_back(expr.name()); }

    void operator()(const UnaryExpression& expr) {
      VisitExpression(*expr.operand(), *this);
    }

    void operator()(const BinaryExpression& expr) {
      VisitExpression(*expr.left_operand(), *this);
      VisitExpression(*expr.right_operand(), *this);
    }

    void operator()(const Expression&) const {}

    std::vector<std::string> fields;
  } visitor;

  VisitExpression(expr, visitor);
  return std::move(visitor.fields);
}

std::vector<std::string> FieldsInExpression(const std::shared_ptr<Expression>& expr) {
  DCHECK_NE(expr, nullptr);
  return FieldsInExpression(*expr);
}

RecordBatchIterator ExpressionEvaluator::FilterBatches(
    RecordBatchIterator unfiltered, std::shared_ptr<Expression> filter) {
  auto filter_batches = [filter, this](const std::shared_ptr<RecordBatch>& unfiltered,
                                       std::shared_ptr<RecordBatch>* filtered,
                                       bool* accept) {
    ARROW_ASSIGN_OR_RAISE(auto selection, Evaluate(*filter, *unfiltered));
    RETURN_NOT_OK(Filter(selection, unfiltered, filtered));
    // drop empty batches
    *accept = (*filtered)->num_rows() > 0;
    return Status::OK();
  };

  return MakeFilterIterator(std::move(filter_batches), std::move(unfiltered));
}

std::shared_ptr<ExpressionEvaluator> ExpressionEvaluator::Null() {
  struct Impl : ExpressionEvaluator {
    Result<Datum> Evaluate(const Expression& expr,
                           const RecordBatch& batch) const override {
      ARROW_ASSIGN_OR_RAISE(auto type, expr.Validate(*batch.schema()));
      std::shared_ptr<Scalar> out;
      RETURN_NOT_OK(MakeNullScalar(type, &out));
      return Datum(std::move(out));
    }

    Result<std::shared_ptr<RecordBatch>> Filter(
        const Datum& selection,
        const std::shared_ptr<RecordBatch>& batch) const override {
      return batch;
    }
  };

  return std::make_shared<Impl>();
}

struct TreeEvaluator::Impl {
  Result<Datum> operator()(const ScalarExpression& expr) const {
    return Datum(expr.value());
  }

  Result<Datum> operator()(const FieldExpression& expr) const {
    if (auto column = batch_.GetColumnByName(expr.name())) {
      return std::move(column);
    }
    return NullDatum();
  }

  Result<Datum> operator()(const AndExpression& expr) const {
    return EvaluateBoolean(expr, compute::KleeneAnd);
  }

  Result<Datum> operator()(const OrExpression& expr) const {
    return EvaluateBoolean(expr, compute::KleeneOr);
  }

  Result<Datum> EvaluateBoolean(const BinaryExpression& expr,
                                Status kernel(compute::FunctionContext* context,
                                              const compute::Datum& left,
                                              const compute::Datum& right,
                                              compute::Datum* out)) const {
    ARROW_ASSIGN_OR_RAISE(auto lhs, this_->Evaluate(*expr.left_operand(), batch_));
    ARROW_ASSIGN_OR_RAISE(auto rhs, this_->Evaluate(*expr.right_operand(), batch_));

    if (lhs.is_scalar()) {
      std::shared_ptr<Array> lhs_array;
      RETURN_NOT_OK(
          MakeArrayFromScalar(pool_, *lhs.scalar(), batch_.num_rows(), &lhs_array));
      lhs = Datum(std::move(lhs_array));
    }

    if (rhs.is_scalar()) {
      std::shared_ptr<Array> rhs_array;
      RETURN_NOT_OK(
          MakeArrayFromScalar(pool_, *rhs.scalar(), batch_.num_rows(), &rhs_array));
      rhs = Datum(std::move(rhs_array));
    }

    Datum out;
    compute::FunctionContext ctx{pool_};
    RETURN_NOT_OK(kernel(&ctx, lhs, rhs, &out));
    return std::move(out);
  }

  Result<Datum> operator()(const NotExpression& expr) const {
    ARROW_ASSIGN_OR_RAISE(auto to_invert, this_->Evaluate(*expr.operand(), batch_));
    if (IsNullDatum(to_invert)) {
      return NullDatum();
    }

    if (to_invert.is_scalar()) {
      bool trivial_condition =
          checked_cast<const BooleanScalar&>(*to_invert.scalar()).value;
      return Datum(std::make_shared<BooleanScalar>(!trivial_condition));
    }

    DCHECK(to_invert.is_array());
    Datum out;
    compute::FunctionContext ctx{pool_};
    RETURN_NOT_OK(arrow::compute::Invert(&ctx, to_invert, &out));
    return std::move(out);
  }

  Result<Datum> operator()(const InExpression& expr) const {
    ARROW_ASSIGN_OR_RAISE(auto operand_values, this_->Evaluate(*expr.operand(), batch_));
    if (IsNullDatum(operand_values)) {
      return Datum(expr.set()->null_count() != 0);
    }

    DCHECK(operand_values.is_array());
    Datum out;
    compute::FunctionContext ctx{pool_};
    RETURN_NOT_OK(arrow::compute::IsIn(&ctx, operand_values, expr.set(), &out));
    return std::move(out);
  }

  Result<Datum> operator()(const IsValidExpression& expr) const {
    ARROW_ASSIGN_OR_RAISE(auto operand_values, this_->Evaluate(*expr.operand(), batch_));
    if (IsNullDatum(operand_values)) {
      return Datum(false);
    }

    if (operand_values.is_scalar()) {
      return Datum(true);
    }

    DCHECK(operand_values.is_array());
    if (operand_values.array()->GetNullCount() == 0) {
      return Datum(true);
    }

    return Datum(std::make_shared<BooleanArray>(operand_values.array()->length,
                                                operand_values.array()->buffers[0]));
  }

  Result<Datum> operator()(const CastExpression& expr) const {
    ARROW_ASSIGN_OR_RAISE(auto to_type, expr.Validate(*batch_.schema()));

    ARROW_ASSIGN_OR_RAISE(auto to_cast, this_->Evaluate(*expr.operand(), batch_));
    if (to_cast.is_scalar()) {
      return to_cast.scalar()->CastTo(to_type);
    }

    DCHECK(to_cast.is_array());
    Datum out;
    compute::FunctionContext ctx{pool_};
    RETURN_NOT_OK(arrow::compute::Cast(&ctx, to_cast, to_type, expr.options(), &out));
    return std::move(out);
  }

  Result<Datum> operator()(const ComparisonExpression& expr) const {
    ARROW_ASSIGN_OR_RAISE(auto lhs, this_->Evaluate(*expr.left_operand(), batch_));
    ARROW_ASSIGN_OR_RAISE(auto rhs, this_->Evaluate(*expr.right_operand(), batch_));

    if (IsNullDatum(lhs) || IsNullDatum(rhs)) {
      return Datum(std::make_shared<BooleanScalar>());
    }

    DCHECK(lhs.is_array());

    Datum out;
    compute::FunctionContext ctx{pool_};
    RETURN_NOT_OK(arrow::compute::Compare(
        &ctx, lhs, rhs, arrow::compute::CompareOptions(expr.op()), &out));
    return std::move(out);
  }

  Result<Datum> operator()(const Expression& expr) const {
    return Status::NotImplemented("evaluation of ", expr.ToString());
  }

  const TreeEvaluator* this_;
  const RecordBatch& batch_;
  MemoryPool* pool_;
};

Result<Datum> TreeEvaluator::Evaluate(const Expression& expr,
                                      const RecordBatch& batch) const {
  return VisitExpression(expr, Impl{this, batch, pool_});
}

Result<std::shared_ptr<RecordBatch>> TreeEvaluator::Filter(
    const compute::Datum& selection, const std::shared_ptr<RecordBatch>& batch) const {
  if (selection.is_array()) {
    auto selection_array = selection.make_array();
    std::shared_ptr<RecordBatch> filtered;
    compute::FunctionContext ctx{pool_};
    RETURN_NOT_OK(compute::Filter(&ctx, *batch, *selection_array, &filtered));
    return std::move(filtered);
  }

  if (!selection.is_scalar() || selection.type()->id() != Type::BOOL) {
    return Status::NotImplemented("Filtering batches against DatumKind::",
                                  selection.kind(), " of type ", *selection.type());
  }

  if (BooleanScalar(true).Equals(selection.scalar())) {
    return batch;
  }

  return batch->Slice(0, 0);
}

}  // namespace dataset
}  // namespace arrow
