// Copyright (c) 2021 CINN Authors. All Rights Reserved.
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

//! @h1 C++ DSL API tutorial: Matrix Multiplication
//! This tutorial will guide you through the basic usage of the C++ DSL API.

#include <gtest/gtest.h>

#include "cinn/cinn.h"

using namespace cinn;  // NOLINT

//! @IGNORE-NEXT
TEST(matmul, basic) {
  //! @h2 Computation basic defition
  //! Declare some varialbe for latter usages.
  Expr M(100), N(200), K(50);
  Var k(K, "k0");  // the reduce axis

  //! Placeholder represents the input arguments for a computation.
  Placeholder<float> A("A", {M, K});
  Placeholder<float> B("B", {K, N});

  //! Define a computation to get the result tensor C.
  auto C = Compute(
      {M, N} /*domain*/, [=](Expr i, Expr j) { return ReduceSum(A(i, k) * B(k, j), {k} /*reduce axis*/); }, "C");

  //! Generate the stages to get the default schedules.
  auto stages = CreateStages({C} /*the endpoints*/);

  //! Print the generated IR, the `Lower` method just map a computation to the underlying CINN IR.
  auto fn = Lower("fn0", stages, {A, B, C} /*argument list of the fn*/);
  LOG(INFO) << "fn0:\n" << fn;
  //! This will generate the code like
  //! @ROC[c++]
  auto target_source = R"ROC(
function fn0 (_A, _B, _C)
{
  serial for (i, 0, 100)
  {
    serial for (j, 0, 200)
    {
      C__reduce_init[i, j] = 0.00000f
      serial for (k0, 0, 50)
      {
        C[i, j] = (C[i, j] + (A[i, k0] * B[k0, j]))
      }
    }
  }
}
)ROC";

  //! @IGNORE-NEXT
  ASSERT_EQ(utils::GetStreamCnt(fn), utils::Trim(target_source));

  //! Print the IR as C code
  Target x;
  CodeGenC codegen(x);
  Module::Builder builder("module0", Target());
  builder.AddFunction(fn);
  codegen.SetInlineBuiltinCodes(false);  // Disable inserting the predefined runtime codes to the generated code.
  std::string C_source = codegen.Compile(builder.Build(), CodeGenC::OutputKind::CImpl);
  //! @IGNORE-NEXT
  LOG(INFO) << "C:\n" << C_source;
  //! and will get some code like
  //! @ROC[c++]
  target_source = R"ROC(
#include <cinn_runtime.h>
#include <stdio.h>

void fn0(void* _args, int32_t num_args)
{
  const cinn_buffer_t* _A = cinn_pod_value_to_buffer_p(&(((cinn_pod_value_t*)(_args))[0]));
  const cinn_buffer_t* _B = cinn_pod_value_to_buffer_p(&(((cinn_pod_value_t*)(_args))[1]));
  cinn_buffer_t* _C = cinn_pod_value_to_buffer_p(&(((cinn_pod_value_t*)(_args))[2]));
  cinn_buffer_malloc((void*)(0), _C);
  const float* A = ((const float*)(_A->memory));
  const float* B = ((const float*)(_B->memory));
  float* C = ((float*)(_C->memory));
  float* C__reduce_init = ((float*)(_C->memory));
  for (int32_t i = 0; i < 100; i += 1) {
    for (int32_t j = 0; j < 200; j += 1) {
      C__reduce_init[((200 * i) + j)] = 0.00000f;
      for (int32_t k0 = 0; k0 < 50; k0 += 1) {
        C[((200 * i) + j)] = (C[((200 * i) + j)] + (A[((50 * i) + k0)] * B[((200 * k0) + j)]));
      };
    };
  };
  cinn_buffer_free((void*)(0), _C);
}
  )ROC";

  //! @IGNORE-NEXT
  ASSERT_EQ(utils::Trim(C_source), utils::Trim(target_source));

  //! @h2 Basic schedule
  //! The computation defines the basic way to compute the result while the schedules will guide the system to generate
  //! different codes. Each kind of code will result in different performance.

  //! Lets create a new stages to hold some schedules.
  auto stages1 = CreateStages({C});

  //! `Tile` method will split the 0-th and 1-th axis tile by tile of 4.
  stages1[C]->Tile(0, 1, 4, 4);

  //! The newly generated code is as follows
  //! @IGNORE-NEXT
  auto fn1 = Lower("fn1", stages1, {A, B, C});
  //! @IGNORE-NEXT
  LOG(INFO) << "fn1:\n" << fn1;

  //! @ROC[c++]
  target_source = R"ROC(
function fn1 (_A, _B, _C)
{
  serial for (i_outer, 0, 25)
  {
    serial for (i_inner, 0, 4)
    {
      serial for (j_outer, 0, 50)
      {
        serial for (j_inner, 0, 4)
        {
          C__reduce_init[((4 * i_outer) + i_inner), ((4 * j_outer) + j_inner)] = 0.00000f
          serial for (k0, 0, 50)
          {
            C[((4 * i_outer) + i_inner), ((4 * j_outer) + j_inner)] = (C[((4 * i_outer) + i_inner), ((4 * j_outer) + j_inner)] + (A[((4 * i_outer) + i_inner), k0] * B[k0, ((4 * j_outer) + j_inner)]))
          }
        }
      }
    }
  }
})ROC";

  //! @IGNORE-NEXT
  ASSERT_EQ(utils::GetStreamCnt(fn1), utils::Trim(target_source));
}
