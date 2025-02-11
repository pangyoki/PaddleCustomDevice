// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
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

#include "kernels/funcs/npu_funcs.h"
#include "kernels/funcs/npu_op_runner.h"

namespace custom_kernel {

template <typename T, typename Context>
void SubtractRawKernel(const Context& dev_ctx,
                       const phi::DenseTensor& x,
                       const phi::DenseTensor& y,
                       int axis,
                       phi::DenseTensor* out) {
  dev_ctx.template Alloc<T>(out);
  auto stream = dev_ctx.stream();

  const auto& runner = NpuOpRunner("Sub", {x, y}, {*out}, {});
  runner.Run(stream);
}

template <typename T, typename Context>
void SubtractKernel(const Context& dev_ctx,
                    const phi::DenseTensor& x,
                    const phi::DenseTensor& y,
                    phi::DenseTensor* out) {
  int axis = -1;
  custom_kernel::SubtractRawKernel<T>(dev_ctx, x, y, axis, out);
}

template <typename T, typename Context>
void SubtractGradKernel(const Context& dev_ctx,
                        const phi::DenseTensor& x,
                        const phi::DenseTensor& y,
                        const phi::DenseTensor& dout,
                        int axis,
                        phi::DenseTensor* dx,
                        phi::DenseTensor* dy) {
  auto stream = dev_ctx.stream();

  // NOTE(zhiqiu): It seems npu Sub follow the broadcast sematics with
  // default axis=-1?
  // So, the sub_grad should do reduce if needed.
  // For example, the shape of each variable in elementwise_sub:
  // x, dx: [2, 3, 5]
  // y, dy: [1, 5]
  // out, dout: [2, 3, 5]
  // Then, out = x - y  =>  dx = dout, dy = -dout
  // And, the shape of dy can be computed by two stages reduce,
  // 1. [2, 3, 5] => [3, 5], ReduceSumD on axis = 0, keep_dims = false.
  // 2. [3, 5] => [1, 5], ReduceSumD on axis = 0, keep_dims = true.

  if (dx) {
    dev_ctx.template Alloc<T>(dx);
    // For dx
    // stage 1
    auto reduce_ndim = dout.dims().size() - dx->dims().size();
    std::vector<int> axes;
    for (auto i = 0; i < reduce_ndim; ++i) {
      axes.push_back(i);
    }
    phi::DenseTensor* tmp_dout = const_cast<phi::DenseTensor*>(&dout);
    phi::DenseTensor reduced_dout;
    if (axes.size() != 0) {
      std::vector<int64_t> reduced_dout_dims;
      for (auto i = reduce_ndim; i < dout.dims().size(); ++i) {
        reduced_dout_dims.push_back(dout.dims()[i]);
      }

      phi::DenseTensorMeta reduced_dout_meta = {
          dx->dtype(), phi::make_ddim(reduced_dout_dims)};
      reduced_dout.set_meta(reduced_dout_meta);
      dev_ctx.template Alloc<T>(&reduced_dout);

      const auto& runner = NpuOpRunner("ReduceSumD",
                                       {dout},
                                       {reduced_dout},
                                       {{"axes", axes}, {"keep_dims", false}});
      runner.Run(stream);
      tmp_dout = &reduced_dout;
    }

    // stage 2
    axes.clear();
    for (auto i = 0; i < dx->dims().size(); ++i) {
      if (dx->dims()[i] == 1) {
        axes.push_back(i);
      }
    }
    if (axes.size() != 0) {
      const auto& runner = NpuOpRunner("ReduceSumD",
                                       {*tmp_dout},
                                       {*dx},
                                       {{"axes", axes}, {"keep_dims", true}});
      runner.Run(stream);
    } else {
      TensorCopy(dev_ctx, *tmp_dout, false, dx);
    }
  }
  if (dy) {
    dev_ctx.template Alloc<T>(dy);
    // For dy
    // stage 1
    auto reduce_ndim = dout.dims().size() - dy->dims().size();
    std::vector<int> axes;
    for (auto i = 0; i < reduce_ndim; ++i) {
      axes.push_back(i);
    }
    phi::DenseTensor* tmp_dout = const_cast<phi::DenseTensor*>(&dout);
    phi::DenseTensor reduced_dy;
    phi::DenseTensor reduced_dout;

    if (axes.size() != 0) {
      std::vector<int64_t> reduced_dout_dims;
      for (auto i = reduce_ndim; i < dout.dims().size(); ++i) {
        reduced_dout_dims.push_back(dout.dims()[i]);
      }

      phi::DenseTensorMeta reduced_dout_meta = {
          dy->dtype(), phi::make_ddim(reduced_dout_dims)};
      reduced_dout.set_meta(reduced_dout_meta);
      dev_ctx.template Alloc<T>(&reduced_dout);

      const auto& runner = NpuOpRunner("ReduceSumD",
                                       {dout},
                                       {reduced_dout},
                                       {{"axes", axes}, {"keep_dims", false}});
      runner.Run(stream);
      tmp_dout = &reduced_dout;
    }

    // stage 2
    axes.clear();
    phi::DenseTensor* tmp_dy = tmp_dout;
    for (auto i = 0; i < dy->dims().size(); ++i) {
      if (dy->dims()[i] == 1) {
        axes.push_back(i);
      }
    }
    if (axes.size() != 0) {
      phi::DenseTensorMeta reduced_dy_meta = {dy->dtype(), dy->dims()};
      reduced_dy.set_meta(reduced_dy_meta);
      dev_ctx.template Alloc<T>(&reduced_dy);

      const auto& runner = NpuOpRunner("ReduceSumD",
                                       {*tmp_dout},
                                       {reduced_dy},
                                       {{"axes", axes}, {"keep_dims", true}});
      runner.Run(stream);
      tmp_dy = &reduced_dy;
    }

    // stage 3, negative
    const auto& runner = NpuOpRunner("Neg", {*tmp_dy}, {*dy}, {});
    runner.Run(stream);
  }
}

}  // namespace custom_kernel

PD_REGISTER_PLUGIN_KERNEL(subtract_raw,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::SubtractRawKernel,
                          float,
                          int64_t,
                          double) {}

PD_REGISTER_PLUGIN_KERNEL(subtract,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::SubtractKernel,
                          float,
                          int64_t,
                          double) {}

PD_REGISTER_PLUGIN_KERNEL(subtract_grad,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::SubtractGradKernel,
                          float,
                          int64_t,
                          double) {}
