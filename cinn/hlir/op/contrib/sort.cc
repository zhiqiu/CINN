// Copyright (c) 2022 CINN Authors. All Rights Reserved.
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

#include "cinn/hlir/op/contrib/sort.h"

#include <gflags/gflags.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cinn/common/cas.h"
#include "cinn/common/common.h"
#include "cinn/common/context.h"
#include "cinn/common/macros.h"
#include "cinn/hlir/framework/node.h"
#include "cinn/hlir/framework/op.h"
#include "cinn/hlir/framework/op_strategy.h"
#include "cinn/hlir/pe/elementwise.h"
#include "cinn/hlir/pe/ir_schedule_pe.h"
#include "cinn/hlir/pe/transform.h"
#include "cinn/ir/ir.h"
#include "cinn/ir/ir_base.h"
#include "cinn/ir/tensor.h"
#include "cinn/lang/builtin.h"
#include "cinn/lang/compute.h"

DECLARE_bool(cinn_ir_schedule);

namespace cinn {
namespace hlir {
namespace op {

using common::CINNValue;
using common::CINNValuePack;

ir::Tensor ArgSort(const ir::Tensor &A,
                   const common::Target &target,
                   poly::StageMap stages,
                   const int &axis,
                   const bool &is_ascend,
                   const std::string &name) {
  std::string find_func_name;
  std::string index_func_name;
  if (target.arch == common::Target::Arch::NVGPU) {
    index_func_name.assign("cinn_cuda_");
    find_func_name.assign("cinn_cuda_find_int_nd");
  } else if (target.arch == common::Target::Arch::X86) {
    index_func_name.assign("cinn_host_");
    find_func_name.assign("cinn_host_find_int_nd");
  } else {
    LOG(FATAL) << "ArgSort only supports X86 and NVGPU ! Please Check.\n";
  }
  if (is_ascend) {
    index_func_name.append("lt_num_float");
  } else {
    index_func_name.append("gt_num_float");
  }
  int pos_axis = axis;
  if (pos_axis < 0) {
    pos_axis += A->shape.size();
  }
  auto positions = Compute(
      A->shape,
      [=](const std::vector<Expr> &indices) {
        Expr offset(0);
        Expr stride(1);
        for (int i = 0; i < indices.size(); i++) {
          if (i < pos_axis) {
            offset = offset * A->shape[i] + indices[i];
          } else if (i == pos_axis) {
            offset = offset * A->shape[i];
          } else {
            offset = offset * A->shape[i] + indices[i];
            stride = stride * A->shape[i];
          }
        }
        offset            = common::AutoSimplify(offset);
        stride            = common::AutoSimplify(stride);
        auto A_shape_axis = A->shape[pos_axis];
        return lang::CallExtern(index_func_name, {A, A_shape_axis, A(indices), offset, stride});
      },
      name + "_temp");
  auto res = Compute(
      A->shape,
      [=](const std::vector<Expr> &indices) {
        Expr offset(0);
        Expr stride(1);
        for (int i = 0; i < indices.size(); i++) {
          if (i < pos_axis) {
            offset = offset * A->shape[i] + indices[i];
          } else if (i == pos_axis) {
            offset = offset * A->shape[i];
          } else {
            offset = offset * A->shape[i] + indices[i];
            stride = stride * A->shape[i];
          }
        }
        offset = common::AutoSimplify(offset);
        stride = common::AutoSimplify(stride);

        auto A_shape_axis = A->shape[pos_axis];
        auto idx = lang::CallExtern(find_func_name, {positions, A_shape_axis, indices[pos_axis], offset, stride});
        return idx;
      },
      name);
  stages->InsertLazily(positions);
  return res;
}

ir::Tensor Sort(const ir::Tensor &A,
                const common::Target &target,
                poly::StageMap stages,
                const int &axis,
                const bool &is_ascend,
                const std::string &name) {
  int pos_axis = axis;
  if (pos_axis < 0) {
    pos_axis += A->shape.size();
  }
  auto sort_index = ArgSort(A, target, stages, pos_axis, is_ascend, name + "_index");
  auto res        = Compute(
      A->shape,
      [=](const std::vector<Expr> &indices) {
        std::vector<Expr> A_indices(indices);
        A_indices[pos_axis] = sort_index(indices);
        return A(A_indices);
      },
      name);
  stages->InsertLazily(sort_index);
  return res;
}

std::shared_ptr<framework::OpStrategy> StrategyForSort(const framework::NodeAttr &attrs,
                                                       const std::vector<ir::Tensor> &inputs,
                                                       const std::vector<Type> &out_type,
                                                       const std::vector<std::vector<int>> &output_shapes,
                                                       const Target &target) {
  auto attr_store = attrs.attr_store;
  std::string op_name("sort");

  CHECK(attr_store.count("axis")) << "find no attr of axis";
  int axis       = absl::get<int>(attr_store.at("axis"));
  bool is_ascend = true;
  if (attr_store.count("is_ascend")) {
    is_ascend = absl::get<bool>(attr_store.at("is_ascend"));
  }

  framework::CINNCompute sort_compute([=](lang::Args args, lang::RetValue *ret) {
    CHECK(!args.empty()) << "The input arguments of Sort compute is empty! Please check.\n";
    CINNValuePack pack_args = args[0];
    CHECK_GE(pack_args.size(), 1U) << "At least 1 input tensors for Sort compute\n";
    Expr A = pack_args[0];
    CHECK(A.as_tensor());
    CHECK(!output_shapes.empty());
    auto tensor_A = A.as_tensor_ref();
    auto stages   = CreateStages({tensor_A});
    VLOG(3) << "A shape: " << utils::Join(tensor_A->shape, ", ")
            << ", output_shapes: " << utils::Join(output_shapes[0], ", ");
    auto tensor_name = UniqName("Sort_out");
    if (FLAGS_cinn_ir_schedule) {
      CHECK_EQ(pack_args.size(), 2U);
      CHECK(pack_args[1].is_string());
      tensor_name = pack_args[1].operator std::string();
    }
    ir::Tensor out = Sort(tensor_A, target, stages, axis, is_ascend, tensor_name);
    std::vector<CINNValue> res;
    stages->InsertLazily(out);
    res.push_back(CINNValue(out));
    CHECK(!out_type.empty()) << "Output type of Sort is empty! Please check.\n";
    res.push_back(CINNValue(stages));
    *ret = CINNValuePack{res};
  });

  framework::CINNSchedule sort_schedule([=](lang::Args args, lang::RetValue *ret) {
    if (FLAGS_cinn_ir_schedule) {
      CHECK(!args.empty()) << "The input argument of sort_schedule is empty! Please check.\n";
      common::CINNValuePack arg_pack = args[0];
      std::vector<Expr> vec_ast;
      for (int i = 0; i < arg_pack.size(); i++) {
        if (arg_pack[i].is_expr()) {
          Expr temp = arg_pack[i];
          vec_ast.emplace_back(temp);
        }
      }
      CHECK(!vec_ast.empty());
      ir::ModuleExpr mod_expr(vec_ast);
      ir::IRSchedule ir_sch(mod_expr);
      ir_sch.MergeExprs();
      long prod_size = std::accumulate(output_shapes[0].begin(), output_shapes[0].end(), 1, std::multiplies<int>());
      if (prod_size > 1) {
        if (target.arch == Target::Arch::NVGPU) {
          pe::IRCudaScheduleInjective(ir_sch, output_shapes.front(), target);
        } else if (target.arch == Target::Arch::X86) {
          pe::IRScheduleInjectiveCPU(ir_sch, output_shapes.front(), target, true);
        }
      }
      std::vector<common::CINNValue> res{common::CINNValue(ir_sch.GetModule().GetExprs().at(0))};
      *ret = common::CINNValuePack{res};
    } else {
      CHECK(!args.empty()) << "The input argument of sort_schedule is empty! Please check.\n";
      CINNValuePack arg_pack = args[0];
      Expr out               = arg_pack[0];
      CHECK(out.as_tensor());
      *ret = arg_pack;
    }
  });

  auto strategy = std::make_shared<framework::OpStrategy>();
  strategy->AddImpl(sort_compute, sort_schedule, "strategy.sort.x86", 1);
  return strategy;
}

std::shared_ptr<framework::OpStrategy> StrategyForArgSort(const framework::NodeAttr &attrs,
                                                          const std::vector<ir::Tensor> &inputs,
                                                          const std::vector<Type> &out_type,
                                                          const std::vector<std::vector<int>> &output_shapes,
                                                          const Target &target) {
  auto attr_store = attrs.attr_store;
  CHECK(attr_store.count("axis")) << "find no attr of axis";
  int axis       = absl::get<int>(attr_store.at("axis"));
  bool is_ascend = true;
  if (attr_store.count("is_ascend")) {
    is_ascend = absl::get<bool>(attr_store.at("is_ascend"));
  }

  framework::CINNCompute argsort_compute([=](lang::Args args, lang::RetValue *ret) {
    CHECK(!args.empty()) << "The input arguments of ArgSort compute is empty! Please check.\n";
    CINNValuePack pack_args = args[0];
    CHECK_GE(pack_args.size(), 1U) << "At least 1 input tensors for ArgSort compute\n";
    Expr A = pack_args[0];
    CHECK(A.as_tensor());
    CHECK(!output_shapes.empty());
    auto tensor_A = A.as_tensor_ref();
    auto stages   = CreateStages({tensor_A});
    VLOG(3) << "A shape: " << utils::Join(tensor_A->shape, ", ")
            << ", output_shapes: " << utils::Join(output_shapes[0], ", ");
    auto tensor_name = UniqName("ArgSort_out");
    if (FLAGS_cinn_ir_schedule) {
      CHECK_EQ(pack_args.size(), 2U);
      CHECK(pack_args[1].is_string());
      tensor_name = pack_args[1].operator std::string();
    }
    ir::Tensor out = ArgSort(tensor_A, target, stages, axis, is_ascend, tensor_name);
    std::vector<CINNValue> res;
    stages->InsertLazily(out);
    res.push_back(CINNValue(out));
    CHECK(!out_type.empty()) << "Output type of ArgSort is empty! Please check.\n";
    res.push_back(CINNValue(stages));
    *ret = CINNValuePack{res};
  });

  framework::CINNSchedule argsort_schedule([=](lang::Args args, lang::RetValue *ret) {
    if (FLAGS_cinn_ir_schedule) {
      CHECK(!args.empty()) << "The input argument of argsort_schedule is empty! Please check.\n";
      common::CINNValuePack arg_pack = args[0];
      std::vector<Expr> vec_ast;
      for (int i = 0; i < arg_pack.size(); i++) {
        if (arg_pack[i].is_expr()) {
          Expr temp = arg_pack[i];
          vec_ast.emplace_back(temp);
        }
      }
      CHECK(!vec_ast.empty());
      ir::ModuleExpr mod_expr(vec_ast);
      ir::IRSchedule ir_sch(mod_expr);
      ir_sch.MergeExprs();
      long prod_size = std::accumulate(output_shapes[0].begin(), output_shapes[0].end(), 1, std::multiplies<int>());
      if (prod_size > 1) {
        if (target.arch == Target::Arch::NVGPU) {
          pe::IRCudaScheduleInjective(ir_sch, output_shapes.front(), target);
        } else if (target.arch == Target::Arch::X86) {
          pe::IRScheduleInjectiveCPU(ir_sch, output_shapes.front(), target, true);
        }
      }
      std::vector<common::CINNValue> res{common::CINNValue(ir_sch.GetModule().GetExprs().at(0))};
      *ret = common::CINNValuePack{res};
    } else {
      CHECK(!args.empty()) << "The input argument of argsort_schedule is empty! Please check.\n";
      CINNValuePack arg_pack = args[0];
      Expr out               = arg_pack[0];
      CHECK(out.as_tensor());
      *ret = arg_pack;
    }
  });

  auto strategy = std::make_shared<framework::OpStrategy>();
  strategy->AddImpl(argsort_compute, argsort_schedule, "strategy.argsort.x86", 1);
  return strategy;
}

std::vector<std::vector<int>> InferShapeForSort(const std::vector<std::vector<int>> &inputs_shape,
                                                const framework::AttrMapType &attrs) {
  CHECK_EQ(inputs_shape.size(), 1UL) << "The input's shape size should be 1! Please check again.";
  int axis = 0;
  for (auto &iter : attrs) {
    if (iter.first == "axis") {
      axis = absl::get<int>(iter.second);
      break;
    }
  }
  CHECK_GT(inputs_shape[0].size(), axis) << "The input's dim should be greater than axis! ";
  std::vector<std::vector<int>> res{inputs_shape[0]};
  return res;
}

std::vector<Type> InferDtypeForSort(const std::vector<Type> &inputs_type, const framework::AttrMapType &attrs) {
  CHECK_EQ(inputs_type.size(), 1UL) << "The input's type size should be 1! Please check again.";
  std::vector<Type> res{inputs_type[0]};
  return res;
}

std::vector<Type> InferDtypeForArgSort(const std::vector<Type> &inputs_type, const framework::AttrMapType &attrs) {
  CHECK_EQ(inputs_type.size(), 1UL) << "The input's type size should be 1! Please check again.";
  return {Int(32)};
}

}  // namespace op
}  // namespace hlir
}  // namespace cinn

CINN_REGISTER_HELPER(sort_ops) {
  CINN_REGISTER_OP(sort)
      .describe("Sort a variable x along the given axis and return sorted Variable.")
      .set_num_inputs(1)
      .set_num_outputs(1)
      .set_attr<cinn::hlir::framework::StrategyFunction>("CINNStrategy", cinn::hlir::op::StrategyForSort)
      .set_attr("infershape", MakeOpFunction(cinn::hlir::op::InferShapeForSort))
      .set_attr("inferdtype", MakeOpFunction(cinn::hlir::op::InferDtypeForSort))
      .set_support_level(4);

  CINN_REGISTER_OP(argsort)
      .describe("Sort a variable x along the given axis and return indices.")
      .set_num_inputs(1)
      .set_num_outputs(1)
      .set_attr<cinn::hlir::framework::StrategyFunction>("CINNStrategy", cinn::hlir::op::StrategyForArgSort)
      .set_attr("infershape", MakeOpFunction(cinn::hlir::op::InferShapeForSort))
      .set_attr("inferdtype", MakeOpFunction(cinn::hlir::op::InferDtypeForArgSort))
      .set_support_level(4);

  return true;
}
