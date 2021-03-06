/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file expr_operator.cc
 */
#include <tvm/base.h>
#include <tvm/ir.h>
#include <tvm/expr_operator.h>
#include <cmath>
// Centralized header for constant folders.
#include "../arithmetic/const_fold.h"

namespace tvm {

// simple cast that only checks if type matches and cast
inline PrimExpr SimpleCast(const DataType& t, PrimExpr value) {
  if (value.dtype() == t) return value;
  return ir::CastNode::make(t, value);
}

// The public function with a quick checking path.
void BinaryOpMatchTypes(PrimExpr& lhs, PrimExpr& rhs) {  // NOLINT(*)
  if (lhs.dtype() == rhs.dtype()) return;
  DataType ltype = lhs.dtype();
  DataType rtype = rhs.dtype();
  if (ltype.lanes() == 1 && rtype.lanes() != 1) {
    lhs = ir::BroadcastNode::make(lhs, rtype.lanes());
  } else if (rtype.lanes() == 1 && ltype.lanes() != 1) {
    rhs = ir::BroadcastNode::make(rhs, ltype.lanes());
  } else {
    CHECK(ltype.lanes() == rtype.lanes())
        << "Cannot match type " << ltype << " vs " << rtype;
  }
  if (lhs.dtype() == rhs.dtype()) return;
  // Only do very simple type coversion
  // int->float, DataType::Int(32)->int(64)
  // require the types to be relatively consistent
  // This will the reduce amount code generated by operators
  // and also help user to find potential type conversion problems.
  if (!lhs.dtype().is_float() && rhs.dtype().is_float()) {
    // int->float
    lhs = cast(rhs.dtype(), lhs);
  } else if (lhs.dtype().is_float() && !rhs.dtype().is_float()) {
    // int->float
    rhs = cast(lhs.dtype(), rhs);
  } else if ((lhs.dtype().is_int() && rhs.dtype().is_int()) ||
             (lhs.dtype().is_uint() && rhs.dtype().is_uint())) {
    // promote int to higher bits
    if (lhs.dtype().bits() < rhs.dtype().bits()) {
      lhs = cast(rhs.dtype(), lhs);
    } else {
      rhs = cast(lhs.dtype(), rhs);
    }
  } else if ((lhs.dtype().is_int() && rhs.dtype().is_uint()) ||
             (lhs.dtype().is_uint() && rhs.dtype().is_int())) {
    int bits = std::max(lhs.dtype().bits(), rhs.dtype().bits());
    lhs = SimpleCast(DataType::Int(bits, lhs.dtype().lanes()), lhs);
    rhs = SimpleCast(DataType::Int(bits, rhs.dtype().lanes()), rhs);
  } else {
    LOG(FATAL) << "Cannot match type " << ltype << " vs " << rtype;
  }
}


// maximum and min limits
PrimExpr max_value(const DataType& dtype) {
  using namespace ir;
  CHECK_EQ(dtype.lanes(), 1);
  if (dtype.is_int()) {
    if (dtype.bits() == 64) {
      return IntImmNode::make(dtype, std::numeric_limits<int64_t>::max());
    } else if (dtype.bits() < 64) {
      int64_t val = 1;
      val = (val << (dtype.bits() - 1)) - 1;
      return IntImmNode::make(dtype, val);
    }
  } else if (dtype.is_uint()) {
    if (dtype.bits() == 64) {
      return UIntImmNode::make(dtype, std::numeric_limits<uint64_t>::max());
    } else if (dtype.bits() < 64) {
      uint64_t val = 1;
      val = (val << static_cast<uint64_t>(dtype.bits())) - 1;
      return UIntImmNode::make(dtype, val);
    }
  } else if (dtype.is_float()) {
    if (dtype.bits() == 64) {
      return FloatImmNode::make(dtype, std::numeric_limits<double>::max());
    } else if (dtype.bits() == 32) {
      return FloatImmNode::make(dtype, std::numeric_limits<float>::max());
    } else if (dtype.bits() == 16) {
      return FloatImmNode::make(dtype, 65504.0);
    }
  }
  LOG(FATAL) << "Cannot decide max_value for type" << dtype;
  return PrimExpr();
}

PrimExpr min_value(const DataType& dtype) {
  using namespace ir;
  CHECK_EQ(dtype.lanes(), 1);
  if (dtype.is_int()) {
    if (dtype.bits() == 64) {
      return IntImmNode::make(dtype, std::numeric_limits<int64_t>::lowest());
    } else if (dtype.bits() < 64) {
      int64_t val = 1;
      val = -(val << (dtype.bits() - 1));
      return IntImmNode::make(dtype, val);
    }
  } else if (dtype.is_uint()) {
    return UIntImmNode::make(dtype, 0);
  } else if (dtype.is_float()) {
    if (dtype.bits() == 64) {
      return FloatImmNode::make(dtype, std::numeric_limits<double>::lowest());
    } else if (dtype.bits() == 32) {
      return FloatImmNode::make(dtype, std::numeric_limits<float>::lowest());
    } else if (dtype.bits() == 16) {
      return FloatImmNode::make(dtype, -65504.0);
    }
  }
  LOG(FATAL) << "Cannot decide min_value for type" << dtype;
  return PrimExpr();
}

template<typename ValueType>
inline bool ConstPowerHelper(ValueType val, int *shift) {
  if (val <= 0) return false;
  shift[0] = 0;
  while (val != 0) {
    if (val & 1) {
      return (val == 1);
    }
    ++shift[0];
    val = val >> 1;
  }
  return true;
}

bool is_const_power_of_two_integer(const PrimExpr& x, int* shift) {
  if (const auto* op = x.as<ir::IntImmNode>()) {
    return ConstPowerHelper(op->value, shift);
  } else if (const auto* op = x.as<ir::UIntImmNode>()) {
    return ConstPowerHelper(op->value, shift);
  } else {
    return false;
  }
}

PrimExpr cast(const DataType& t, PrimExpr value) {
  using ir::IntImmNode;
  using ir::UIntImmNode;
  using ir::FloatImmNode;
  if (value.dtype() == t) return value;
  // const fold IntImm as they are used in index computations
  if (t.lanes() == 1) {
    if (const IntImmNode* op = value.as<IntImmNode>()) {
      return make_const(t, op->value);
    } else if (const UIntImmNode* op = value.as<UIntImmNode>()) {
      return make_const(t, op->value);
    } else if (const FloatImmNode* op = value.as<FloatImmNode>()) {
      return make_const(t, op->value);
    }
    return ir::CastNode::make(t, value);
  } else {
    if (value.dtype().lanes() == 1) {
      // manually unroll cast
      DataType vtype = t.element_of();
      if (value.dtype() != vtype) {
        if (const IntImmNode* op = value.as<IntImmNode>()) {
          value = make_const(vtype, op->value);
        } else if (const UIntImmNode* op = value.as<UIntImmNode>()) {
          return make_const(t, op->value);
        } else if (const FloatImmNode* op = value.as<FloatImmNode>()) {
          value = make_const(vtype, op->value);
        } else {
          value = ir::CastNode::make(vtype, value);
        }
      }
      return ir::BroadcastNode::make(value, t.lanes());
    } else {
      CHECK(value.dtype().lanes() == t.lanes());
      return ir::CastNode::make(t, value);
    }
  }
}

PrimExpr reinterpret(const DataType& t, PrimExpr value) {
  if (value.dtype() == t) return value;
  return ir::CallNode::make(
    t, ir::CallNode::reinterpret, { value }, ir::CallNode::PureIntrinsic);
}

PrimExpr operator+(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<ir::AddNode>(a, b);
  if (ret.defined()) return ret;
  return ir::AddNode::make(a, b);
}

// negation
PrimExpr operator-(PrimExpr a) {
  using ir::IntImmNode;
  using ir::FloatImmNode;
  const IntImmNode* pa = a.as<IntImmNode>();
  const FloatImmNode* fa = a.as<FloatImmNode>();
  if (pa) return ir::IntImmNode::make(a.dtype(), -pa->value);
  if (fa) return ir::FloatImmNode::make(a.dtype(), -fa->value);
  return make_zero(a.dtype()) - a;
}

PrimExpr operator-(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<ir::SubNode>(a, b);
  if (ret.defined()) return ret;
  return ir::SubNode::make(a, b);
}

PrimExpr operator*(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<ir::MulNode>(a, b);
  if (ret.defined()) return ret;
  return ir::MulNode::make(a, b);
}

PrimExpr div(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<ir::DivNode>(a, b);
  if (ret.defined()) return ret;
  return ir::DivNode::make(a, b);
}

PrimExpr truncdiv(PrimExpr a, PrimExpr b) {
  CHECK(a.dtype().is_int() || a.dtype().is_uint()) << a;
  CHECK(b.dtype().is_int() || b.dtype().is_uint()) << b;
  return div(a, b);
}

PrimExpr truncmod(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<ir::ModNode>(a, b);
  if (ret.defined()) return ret;
  return ir::ModNode::make(a, b);
}

PrimExpr operator/(PrimExpr a, PrimExpr b) {
  return div(a, b);
}

PrimExpr operator%(PrimExpr a, PrimExpr b) {
  return truncmod(a, b);
}

// TODO(tqchen): switch to floordiv
PrimExpr indexdiv(PrimExpr a, PrimExpr b) {
  return floordiv(a, b);
}

PrimExpr indexmod(PrimExpr a, PrimExpr b) {
  return floormod(a, b);
}

PrimExpr floordiv(PrimExpr a, PrimExpr b) {
  CHECK(a.dtype().is_int() || a.dtype().is_uint()) << a;
  CHECK(b.dtype().is_int() || b.dtype().is_uint()) << b;
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<ir::FloorDivNode>(a, b);
  if (ret.defined()) return ret;
  return ir::FloorDivNode::make(a, b);
}

PrimExpr floormod(PrimExpr a, PrimExpr b) {
  CHECK(a.dtype().is_int() || a.dtype().is_uint()) << a;
  CHECK(b.dtype().is_int() || b.dtype().is_uint()) << b;
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<ir::FloorModNode>(a, b);
  if (ret.defined()) return ret;
  return ir::FloorModNode::make(a, b);
}

PrimExpr min(PrimExpr a, PrimExpr b) {
  // inf-aware simplificaiton
  using arith::is_pos_inf;
  using arith::is_neg_inf;
  if (is_pos_inf(a)) return b;
  if (is_neg_inf(a)) return a;
  if (is_pos_inf(b)) return a;
  if (is_neg_inf(b)) return b;
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<ir::MinNode>(a, b);
  if (ret.defined()) return ret;
  return ir::MinNode::make(a, b);
}

PrimExpr max(PrimExpr a, PrimExpr b) {
  // inf-aware simplificaiton
  using arith::is_pos_inf;
  using arith::is_neg_inf;
  if (is_pos_inf(a)) return a;
  if (is_neg_inf(a)) return b;
  if (is_pos_inf(b)) return b;
  if (is_neg_inf(b)) return a;
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<ir::MaxNode>(a, b);
  if (ret.defined()) return ret;
  return ir::MaxNode::make(a, b);
}

PrimExpr if_then_else(PrimExpr cond, PrimExpr true_value, PrimExpr false_value) {
  using ir::IntImmNode;
  using ir::UIntImmNode;
  CHECK(cond.dtype() == DataType::Bool(1))
      << "if_then_else only accept the condition to be boolean type.";
  BinaryOpMatchTypes(true_value, false_value);
  if (const UIntImmNode* op = cond.as<UIntImmNode>()) {
    if (op->value != 0) {
      return true_value;
    } else {
      return false_value;
    }
  } else if (const IntImmNode* op = cond.as<IntImmNode>()) {
    if (op->value != 0) {
      return true_value;
    } else {
      return false_value;
    }
  }
  return ir::CallNode::make(
      true_value.dtype(),
      ir::intrinsic::tvm_if_then_else,
      {cond, true_value, false_value},
      ir::CallNode::PureIntrinsic);
}

PrimExpr likely(PrimExpr cond) {
  if (is_const(cond)) return cond;
  return ir::CallNode::make(cond.dtype(),
                            ir::CallNode::likely,
                            { cond },
                            ir::CallNode::PureIntrinsic);
}

PrimExpr operator>(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<ir::GTNode>(a, b);
  if (ret.defined()) return ret;
  return ir::GTNode::make(a, b);
}

PrimExpr operator>=(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<ir::GENode>(a, b);
  if (ret.defined()) return ret;
  return ir::GENode::make(a, b);
}

PrimExpr operator<(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<ir::LTNode>(a, b);
  if (ret.defined()) return ret;
  return ir::LTNode::make(a, b);
}

PrimExpr operator<=(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<ir::LENode>(a, b);
  if (ret.defined()) return ret;
  return ir::LENode::make(a, b);
}

PrimExpr operator==(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<ir::EQNode>(a, b);
  if (ret.defined()) return ret;
  return ir::EQNode::make(a, b);
}

PrimExpr operator!=(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<ir::NENode>(a, b);
  if (ret.defined()) return ret;
  return ir::NENode::make(a, b);
}

PrimExpr operator&&(PrimExpr a, PrimExpr b) {
  CHECK(a.dtype().is_bool());
  CHECK(b.dtype().is_bool());
  PrimExpr ret = arith::TryConstFold<ir::AndNode>(a, b);
  if (ret.defined()) return ret;
  return ir::AndNode::make(a, b);
}

PrimExpr operator||(PrimExpr a, PrimExpr b) {
  CHECK(a.dtype().is_bool());
  CHECK(b.dtype().is_bool());
  PrimExpr ret = arith::TryConstFold<ir::OrNode>(a, b);
  if (ret.defined()) return ret;
  return ir::OrNode::make(a, b);
}

PrimExpr operator!(PrimExpr a) {
  CHECK(a.dtype().is_bool());
  PrimExpr ret = arith::TryConstFold<ir::NotNode>(a);
  if (ret.defined()) return ret;
  return ir::NotNode::make(a);
}

PrimExpr operator>>(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  TVM_INDEX_CONST_PROPAGATION({
      const DataType& rtype = a.dtype();
      if (pa && pb) return IntImmNode::make(rtype, (pa->value >> pb->value));
      if (pb) {
        if (pb->value == 0) return a;
      }
    });
  return ir::CallNode::make(
    a.dtype(), ir::CallNode::shift_right, { a, b }, ir::CallNode::PureIntrinsic);
}

PrimExpr operator<<(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  TVM_INDEX_CONST_PROPAGATION({
      const DataType& rtype = a.dtype();
      if (pa && pb) return IntImmNode::make(rtype, (pa->value << pb->value));
      if (pb) {
        if (pb->value == 0) return a;
      }
    });
  return ir::CallNode::make(
    a.dtype(), ir::CallNode::shift_left, { a, b }, ir::CallNode::PureIntrinsic);
}

PrimExpr operator&(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  TVM_INDEX_CONST_PROPAGATION({
      const DataType& rtype = a.dtype();
      if (pa && pb) return IntImmNode::make(rtype, (pa->value & pb->value));
    });
  return ir::CallNode::make(
    a.dtype(), ir::CallNode::bitwise_and, { a, b }, ir::CallNode::PureIntrinsic);
}

PrimExpr operator|(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  TVM_INDEX_CONST_PROPAGATION({
      const DataType& rtype = a.dtype();
      if (pa && pb) return IntImmNode::make(rtype, (pa->value | pb->value));
    });
  return ir::CallNode::make(
    a.dtype(), ir::CallNode::bitwise_or, { a, b }, ir::CallNode::PureIntrinsic);
}

PrimExpr operator^(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  TVM_INDEX_CONST_PROPAGATION({
      const DataType& rtype = a.dtype();
      if (pa && pb) return IntImmNode::make(rtype, (pa->value ^ pb->value));
    });
  return ir::CallNode::make(
    a.dtype(), ir::CallNode::bitwise_xor, { a, b }, ir::CallNode::PureIntrinsic);
}

PrimExpr operator~(PrimExpr a) {
  CHECK(a.dtype().is_int() || a.dtype().is_uint());
  return ir::CallNode::make(
    a.dtype(), ir::CallNode::bitwise_not, { a }, ir::CallNode::PureIntrinsic);
}

PrimExpr pow(PrimExpr x, PrimExpr y) {
  BinaryOpMatchTypes(x, y);
  CHECK(x.dtype().is_float()) << "power only applies to float";
  return ir::CallNode::make(
    x.dtype(), "pow", { x, y }, ir::CallNode::PureIntrinsic);
}

PrimExpr abs(PrimExpr x) {
  if (x.dtype().is_int()) {
    using ir::IntImmNode;
    const IntImmNode* px = x.as<IntImmNode>();
    if (px) {
      return ir::IntImmNode::make(x.dtype(), std::abs(px->value));
    }
    return ir::SelectNode::make(x >= make_zero(x.dtype()), x, -x);
  } else if (x.dtype().is_float()) {
    using ir::FloatImmNode;
    const FloatImmNode* fx = x.as<FloatImmNode>();
    if (fx) {
      return ir::FloatImmNode::make(x.dtype(), std::fabs(fx->value));
    }
    return ir::CallNode::make(x.dtype(), "fabs", {x}, ir::CallNode::PureIntrinsic);
  } else if (x.dtype().is_uint()) {
    return x;
  } else {
    LOG(FATAL) << "Data type " << x.dtype()
               <<" not supported for absolute op. Skipping absolute op...";
    return x;
  }
}

PrimExpr isnan(PrimExpr x) {
  DataType t = DataType::Bool(x.dtype().lanes());
  if (x.dtype().is_int() || x.dtype().is_uint()) {
    return make_const(t, false);
  } else if (x.dtype().is_float()) {
    using ir::FloatImmNode;
    const FloatImmNode* fx = x.as<FloatImmNode>();
    if (fx) {
      return make_const(t, std::isnan(fx->value));
    }
    if (x.dtype().bits() == 16) {
      return ir::CallNode::make(t, ir::CallNode::isnan,
                               {cast(DataType::Float(32, t.lanes()), std::move(x))},
                               ir::CallNode::PureIntrinsic);
    } else {
      return ir::CallNode::make(t, ir::CallNode::isnan, {x}, ir::CallNode::PureIntrinsic);
    }
  } else {
    LOG(FATAL) << "Data type " << x.dtype()
               <<" not supported for isnan op. Skipping isnan op...";
    return x;
  }
}

PrimExpr sum(PrimExpr source, Array<IterVar> rdom) {
  Var x("x", source.dtype()), y("y", source.dtype());
  PrimExpr result = ir::AddNode::make(x, y);
  PrimExpr identity_element = make_zero(source.dtype());
  ir::CommReducer combiner =
    ir::CommReducerNode::make({x}, {y}, {result}, {identity_element});
  return ir::ReduceNode::make(combiner, {source}, rdom, make_const(DataType::Bool(1), true), 0);
}

PrimExpr all(PrimExpr source, Array<IterVar> rdom) {
  CHECK(source.dtype().is_bool());
  Var x("x", source.dtype()), y("y", source.dtype());
  PrimExpr result = ir::AndNode::make(x, y);
  PrimExpr identity_element = make_const(source.dtype(), true);
  ir::CommReducer combiner =
    ir::CommReducerNode::make({x}, {y}, {result}, {identity_element});
  return ir::ReduceNode::make(combiner, {source}, rdom, make_const(DataType::Bool(1), true), 0);
}

PrimExpr any(PrimExpr source, Array<IterVar> rdom) {
  CHECK(source.dtype().is_bool());
  Var x("x", source.dtype()), y("y", source.dtype());
  PrimExpr result = ir::OrNode::make(x, y);
  PrimExpr identity_element = make_const(source.dtype(), false);
  ir::CommReducer combiner =
    ir::CommReducerNode::make({x}, {y}, {result}, {identity_element});
  return ir::ReduceNode::make(combiner, {source}, rdom, make_const(DataType::Bool(1), true), 0);
}

PrimExpr max(PrimExpr source, Array<IterVar> rdom) {
  Var x("x", source.dtype()), y("y", source.dtype());
  PrimExpr result = ir::MaxNode::make(x, y);
  PrimExpr identity_element = min_value(source.dtype());
  ir::CommReducer combiner =
    ir::CommReducerNode::make({x}, {y}, {result}, {identity_element});
  return ir::ReduceNode::make(combiner, {source}, rdom, make_const(DataType::Bool(1), true), 0);
}

PrimExpr min(PrimExpr source, Array<IterVar> rdom) {
  Var x("x", source.dtype()), y("y", source.dtype());
  PrimExpr result = ir::MinNode::make(x, y);
  PrimExpr identity_element = max_value(source.dtype());
  ir::CommReducer combiner =
    ir::CommReducerNode::make({x}, {y}, {result}, {identity_element});
  return ir::ReduceNode::make(combiner, {source}, rdom, make_const(DataType::Bool(1), true), 0);
}

PrimExpr prod(PrimExpr source, Array<IterVar> rdom) {
  Var x("x", source.dtype()), y("y", source.dtype());
  PrimExpr result = ir::MulNode::make(x, y);
  PrimExpr identity_element = make_const(source.dtype(), 1);
  ir::CommReducer combiner =
    ir::CommReducerNode::make({x}, {y}, {result}, {identity_element});
  return ir::ReduceNode::make(combiner, {source}, rdom, make_const(DataType::Bool(1), true), 0);
}

PrimExpr fmod(PrimExpr x, PrimExpr y) {
  BinaryOpMatchTypes(x, y);
  CHECK(x.dtype().is_float()) << "fmod only applies to float";
  return ir::CallNode::make(x.dtype(), "fmod", { x, y }, ir::CallNode::PureIntrinsic);
}

PrimExpr floor(PrimExpr x) {
  using ir::FloatImmNode;
  const FloatImmNode* fx = x.as<FloatImmNode>();
  if (fx) return FloatImmNode::make(x.dtype(), std::floor(fx->value));
  return ir::CallNode::make(x.dtype(), "floor", {x}, ir::CallNode::PureIntrinsic);
}

PrimExpr ceil(PrimExpr x) {
  using ir::FloatImmNode;
  const FloatImmNode* fx = x.as<FloatImmNode>();
  if (fx) return FloatImmNode::make(x.dtype(), std::ceil(fx->value));
  return ir::CallNode::make(x.dtype(), "ceil", {x}, ir::CallNode::PureIntrinsic);
}

PrimExpr round(PrimExpr x) {
  using ir::FloatImmNode;
  const FloatImmNode* fx = x.as<FloatImmNode>();
  if (fx) return FloatImmNode::make(x.dtype(), std::nearbyint(fx->value));
  return ir::CallNode::make(x.dtype(), "round", {x}, ir::CallNode::PureIntrinsic);
}

PrimExpr nearbyint(PrimExpr x) {
  using ir::FloatImmNode;
  const FloatImmNode* fx = x.as<FloatImmNode>();
  if (fx) return FloatImmNode::make(x.dtype(), std::nearbyint(fx->value));
  return ir::CallNode::make(x.dtype(), "nearbyint", {x}, ir::CallNode::PureIntrinsic);
}

PrimExpr trunc(PrimExpr x) {
  using ir::FloatImmNode;
  const FloatImmNode* fx = x.as<FloatImmNode>();
  if (fx) {
    return FloatImmNode::make(x.dtype(), (fx->value < 0 ? std::ceil(fx->value) :
                                     std::floor(fx->value)));
  }
  return ir::CallNode::make(x.dtype(), "trunc", {x}, ir::CallNode::PureIntrinsic);
}

}  // namespace tvm
