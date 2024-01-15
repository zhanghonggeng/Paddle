// Copyright (c) 2023 PaddlePaddle Authors. All Rights Reserved.
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

#pragma once

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <math.h>
#include <vector>

#include "paddle/fluid/primitive/primitive/primitive.h"
#include "paddle/fluid/primitive/type/lazy_tensor.h"
#include "paddle/fluid/primitive/utils/utils.h"

namespace paddle {
namespace primitive {
namespace details {

template <typename T>
void abs_grad(const Tensor& x, const Tensor& out_grad, Tensor* x_grad) {
  if (x_grad) {
    auto sign_tmp = sign<T>(x);
    set_output<T>(out_grad * sign_tmp, x_grad);
  }
}

template <typename T>
void assign_grad(const Tensor& out_grad, Tensor* x_grad) {
  if (x_grad) {
    by_pass<T>(out_grad, x_grad);
  }
}

template <typename T>
void cumsum_grad(const Tensor& x,
                 const Tensor& out_grad,
                 const Scalar& axis,
                 bool flatten,
                 bool exclusive,
                 bool reverse,
                 Tensor* x_grad) {
  if (x_grad) {
    auto grad = cumsum<T>(out_grad, axis, flatten, exclusive, !reverse);
    grad = reshape<T>(grad, x.shape());
    set_output<T>(grad, x_grad);
  }
}

template <typename T>
void divide_grad(const Tensor& x,
                 const Tensor& y,
                 const Tensor& out,
                 const Tensor& out_grad,
                 int axis,
                 Tensor* dx,
                 Tensor* dy) {
  if (dy) {
    // dy = -(x/y^2) * dout
    auto dy_res = -(x / y.pow(2.0)) * out_grad;
    if (x.dims() != y.dims()) {
      // Maybe need reduce here
      phi::DDim reduce_dim = get_reduce_dims(y.dims(), x.dims());
      if (!reduce_dim.size()) {
        set_output<T>(dy_res, dy);
      } else {
        auto dy_reduce_res =
            sum<T>(dy_res, common::vectorize(reduce_dim), y.dtype(), false);
        auto dy_tmp = reshape<T>(dy_reduce_res, common::vectorize(y.dims()));
        set_output<T>(dy_tmp, dy);
      }
    } else {
      set_output<T>(dy_res, dy);
    }
  }  // indicate we will compute dy
  if (dx) {
    // dx = (1/y) * dout
    auto one_tensor = full<T>(common::vectorize(y.dims()), 1.0, y.dtype());
    auto dx_res = one_tensor / y * out_grad;
    if (y.dims() != x.dims()) {
      // Maybe need reduce here
      auto reduce_dim = get_reduce_dims(x.dims(), y.dims());
      if (!reduce_dim.size()) {
        set_output<T>(dx_res, dx);
      } else {
        auto dx_reduce_res =
            sum<T>(dx_res, common::vectorize(reduce_dim), x.dtype(), false);
        auto dx_tmp = reshape<T>(dx_reduce_res, common::vectorize(x.dims()));
        set_output<T>(dx_tmp, dx);
      }

    } else {
      set_output<T>(dx_res, dx);
    }
  }  // indicate we will compute dx
}

template <typename T>
void floor_grad(const Tensor& out_grad, Tensor* x_grad) {
  if (x_grad) {
    auto zero_tensor =
        full<T>(common::vectorize(out_grad.dims()), 0.0, out_grad.dtype());
    set_output<T>(zero_tensor, x_grad);
  }
}

template <typename T>
void sum_grad(const Tensor& x,
              const Tensor& out_grad,
              const IntArray& axis,
              bool keepdim,
              bool reduce_all,
              Tensor* x_grad) {
  if (!x_grad) {
    return;
  }
  std::vector<int64_t> x_dim = common::vectorize<int64_t>(x.dims());
  int64_t axis_size = axis.size();
  int64_t x_dim_size = x_dim.size();
  reduce_all = false;
  if (reduce_all || axis_size == 0 || axis_size == x_dim_size) {
    reduce_all = true;
  } else {
    reduce_all = false;
  }
  auto x_grad_tmp = Tensor();
  if (x_dim_size == 1) {
    x_grad_tmp = expand<T>(out_grad, IntArray(x_dim));
  } else {
    if (!keepdim) {
      auto axis_ = std::vector<int64_t>();
      if (reduce_all) {
        for (int64_t i = 0; i < x_dim_size; i++) {
          axis_.push_back(i);
        }
      } else {
        axis_ = axis.GetData();
        for (int64_t i = 0; i < axis_size; i++) {
          if (axis[i] < 0) {
            axis_[i] = axis[i] + x_dim_size;
          }
        }
      }
      auto out_grad_shape = get_unsqueeze_dims(out_grad, axis_);
      auto out_grad_ = reshape<T>(out_grad, out_grad_shape);
      x_grad_tmp = expand<T>(out_grad_, IntArray(x_dim));
    } else {
      x_grad_tmp = expand<T>(out_grad, IntArray(x_dim));
    }
  }

  set_output<T>(x_grad_tmp, x_grad);
}

template <typename T>
void gelu_grad(const Tensor& x,
               const Tensor& out_grad,
               bool approximate,
               Tensor* x_grad) {
  if (!x_grad) return;
  // Promote to fp32 when the input type is fp16 for keeping consistent with
  // phi kernel

  if (x.dtype() == phi::DataType::FLOAT16 ||
      x.dtype() == phi::DataType::BFLOAT16) {
    auto promoted_x = cast<T>(x, phi::DataType::FLOAT32);
    auto promoted_out_grad = cast<T>(out_grad, phi::DataType::FLOAT32);
    if (approximate) {
      float kbeta = M_SQRT2 * M_2_SQRTPI * 0.5;
      float kkappa = 0.044715;
      auto x_sq = promoted_x * promoted_x;
      auto x_cube = x_sq * promoted_x;
      auto inner = kbeta * (promoted_x + kkappa * x_cube);
      auto tanh_inner = tanh<T>(inner);

      auto left = scale<T>(promoted_x, 0.5);
      auto right = scale<T>(tanh_inner, 1., 1.);

      auto left_derivative = scale<T>(right, 0.5);

      auto tanh_derivative = scale<T>(tanh_inner * tanh_inner, -1., 1.);
      auto inner_derivative = kbeta * (scale<T>(3 * kkappa * x_sq, 1., 1.));
      auto right_derivative = left * tanh_derivative * inner_derivative;

      set_output<T>(
          cast<T>(promoted_out_grad * (left_derivative + right_derivative),
                  x.type()),
          x_grad);
    } else {
      float kalpha = M_SQRT1_2;
      float kbeta = M_2_SQRTPI * M_SQRT1_2 * 0.5;
      auto cdf = scale<T>(scale<T>(erf<T>(kalpha * promoted_x), 1., 1.), 0.5);
      auto pdf = kbeta * exp<T>(scale<T>(promoted_x * promoted_x, -0.5));
      set_output<T>(
          cast<T>(promoted_out_grad * (cdf + promoted_x * pdf), x.type()),
          x_grad);
    }
  } else {
    // Scale only support fp32 attr in static graph mode, use elementwise_xx
    // when precision is over fp32.
    if (approximate) {
      auto kBeta = M_SQRT2 * M_2_SQRTPI * 0.5;
      auto kKappa = 0.044715;
      auto x_sq = x * x;
      auto x_cube = x_sq * x;
      auto inner = kBeta * (x + kKappa * x_cube);
      auto tanh_inner = tanh<T>(inner);

      auto left = scale<T>(x, 0.5);
      auto right = scale<T>(tanh_inner, 1., 1.);

      auto left_derivative = scale<T>(right, 0.5);

      auto tanh_derivative = scale<T>(tanh_inner * tanh_inner, -1., 1.);
      auto inner_derivative = kBeta * (scale<T>(3 * kKappa * x_sq, 1., 1.));
      auto right_derivative = left * tanh_derivative * inner_derivative;

      set_output<T>(out_grad * (left_derivative + right_derivative), x_grad);
    } else {
      auto kAlpha = M_SQRT1_2;
      auto kBeta = M_2_SQRTPI * M_SQRT1_2 * 0.5;
      auto cdf = scale<T>(scale<T>(erf<T>(kAlpha * x), 1., 1.), 0.5);
      auto pdf = kBeta * exp<T>(scale<T>(x * x, -0.5));
      set_output<T>(out_grad * (cdf + x * pdf), x_grad);
    }
  }
}

template <typename T>
void reshape_grad(const Tensor& xshape,
                  const Tensor& grad_out,
                  Tensor* grad_x) {
  if (grad_x) {
    // xshape: [0] + x.shape
    auto xshape_dims = xshape.dims();
    auto x_dims = common::slice_ddim(xshape_dims, 1, xshape_dims.size());
    auto grad_x_tmp = reshape<T>(grad_out, common::vectorize(x_dims));
    set_output<T>(grad_x_tmp, grad_x);
  }
}

template <typename T>
void roll_grad(const Tensor& x,
               const Tensor& out_grad,
               const IntArray& shifts,
               const std::vector<int64_t>& axis,
               Tensor* x_grad) {
  if (x_grad) {
    auto shifts_ = shifts.GetData();
    int64_t nums = shifts_.size();
    for (int64_t i = 0; i < nums; i++) {
      shifts_[i] = 0 - shifts_[i];
    }
    auto x_grad_output = roll<T>(out_grad, shifts_, axis);
    set_output<T>(x_grad_output, x_grad);
  }
}

template <typename T>
void transpose_grad(const Tensor& grad_out,
                    const std::vector<int>& perm,
                    Tensor* grad_x) {
  if (grad_x) {
    std::vector<int> reverse_perm(perm);
    // make origin ranks
    for (int i = 0; i < static_cast<int>(perm.size()); ++i) {
      if (perm[i] >= 0) {
        reverse_perm[perm[i]] = i;
      } else {
        reverse_perm[perm[i] + perm.size()] = i;
      }
    }
    auto grad_x_tmp = transpose<T>(grad_out, reverse_perm);
    set_output<T>(grad_x_tmp, grad_x);
  }
}

template <typename T>
void scatter_grad(const Tensor& index,
                  const Tensor& updates,
                  const Tensor& out_grad,
                  bool overwrite,
                  Tensor* x_grad,
                  Tensor* updates_grad) {
  if (x_grad) {
    auto zero_tensor =
        full<T>(common::vectorize(updates.dims()), 0.0, updates.dtype());
    auto tmp_grad = scatter<T>(out_grad, index, zero_tensor, false);
    set_output<T>(tmp_grad, x_grad);
  }

  if (updates_grad) {
    Scalar tmp_zero = 0;
    auto tmp_updates_grad = gather<T>(out_grad, index, tmp_zero);
    set_output<T>(tmp_updates_grad, updates_grad);
  }
}

template <typename T>
void scatter_nd_add_grad(const Tensor& index,
                         const Tensor& updates,
                         const Tensor& out_grad,
                         Tensor* x_grad,
                         Tensor* updates_grad) {
  if (x_grad) {
    by_pass<T>(out_grad, x_grad);
  }
  if (updates_grad) {
    // Gradient by Gather: dUpdates = dO[Ids]
    auto tmp_updates_grad = gather_nd<T>(out_grad, index);
    set_output<T>(tmp_updates_grad, updates_grad);
  }
}

template <typename T>
void sin_grad(const Tensor& x, const Tensor& out_grad, Tensor* x_grad) {
  auto x_grad_tmp = cos<T>(x) * out_grad;
  set_output<T>(x_grad_tmp, x_grad);
}

template <typename T>
void cos_grad(const Tensor& x, const Tensor& out_grad, Tensor* x_grad) {
  auto x_grad_tmp = -sin<T>(x) * out_grad;
  set_output<T>(x_grad_tmp, x_grad);
}

template <typename T>
void tanh_grad(const Tensor& out, const Tensor& grad_out, Tensor* grad_x) {
  if (!grad_x) return;
  auto grad_x_tmp = grad_out * (1 - out * out);
  set_output<T>(grad_x_tmp, grad_x);
}

template <typename T>
void concat_grad(const std::vector<Tensor>& x,
                 const Tensor& out_grad,
                 const Scalar& axis,
                 std::vector<Tensor*> x_grad) {
  int axis_value = axis.to<int>();
  int rank = x[0].dims().size();
  if (axis_value < 0) {
    axis_value = axis_value + rank;
  }
  axis_value = axis_value > 0 ? axis_value : 0;
  std::vector<int> sections;
  int x_num = x.size();
  for (int i = 0; i < x_num; ++i) {
    sections.push_back(x[i].dims()[axis_value]);
  }
  std::vector<Tensor> x_grad_tmp =
      split<T>(out_grad, IntArray(sections), axis_value);
  for (int i = 0; i < x_num; ++i) {
    if (x_grad[i]) {
      set_output<T>(x_grad_tmp.at(i), x_grad.at(i));
    }
  }
}

template <typename T>
void split_grad(const std::vector<Tensor>& out_grad,
                const Scalar& axis,
                Tensor* x_grad) {
  if (x_grad) {
    auto grad = concat<T>(out_grad, axis);
    set_output<T>(grad, x_grad);
  }
}

template <typename T>
void cast_grad(const Tensor& x, const Tensor& out_grad, Tensor* x_grad) {
  if (x_grad) {
    auto res = cast<T>(out_grad, x.dtype());
    set_output<T>(res, x_grad);
  }
}

template <typename T>
void add_grad(const Tensor& x,
              const Tensor& y,
              const Tensor& out_grad,
              int axis,
              Tensor* dx,
              Tensor* dy) {
  if (dy) {
    if (x.dims() != y.dims()) {
      // Maybe need reduce here
      phi::DDim reduce_dim = get_reduce_dims(y.dims(), x.dims());
      if (!reduce_dim.size()) {
        by_pass<T>(out_grad, dy);
      } else {
        auto dy_reduce_res =
            out_grad.sum(common::vectorize(reduce_dim), y.dtype(), false);
        auto dy_tmp = reshape<T>(dy_reduce_res, common::vectorize(y.dims()));
        set_output<T>(dy_tmp, dy);
      }

    } else {
      by_pass<T>(out_grad, dy);
    }
  }
  if (dx) {
    if (y.dims() != x.dims()) {
      // Maybe need reduce here
      auto reduce_dim = get_reduce_dims(x.dims(), y.dims());
      if (!reduce_dim.size()) {
        by_pass<T>(out_grad, dx);
      } else {
        auto dx_reduce_res =
            out_grad.sum(common::vectorize(reduce_dim), x.dtype(), false);
        auto dx_tmp = reshape<T>(dx_reduce_res, common::vectorize(x.dims()));
        set_output<T>(dx_tmp, dx);
      }
    } else {
      by_pass<T>(out_grad, dx);
    }
  }
}

template <typename T>
void subtract_grad(const Tensor& x,
                   const Tensor& y,
                   const Tensor& out_grad,
                   int axis,
                   Tensor* dx,
                   Tensor* dy) {
  if (dy) {
    auto scale_out_grad = scale<T>(out_grad, -1.0, 0.0, true);
    if (x.dims() != y.dims()) {
      // Maybe need reduce here
      phi::DDim reduce_dim = get_reduce_dims(y.dims(), x.dims());
      if (!reduce_dim.size()) {
        by_pass<T>(scale_out_grad, dy);
      } else {
        auto dy_reduce_res =
            scale_out_grad.sum(common::vectorize(reduce_dim), y.dtype(), false);
        auto dy_tmp = reshape<T>(dy_reduce_res, common::vectorize(y.dims()));
        set_output<T>(dy_tmp, dy);
      }
    } else {
      by_pass<T>(scale_out_grad, dy);
    }
  }
  if (dx) {
    if (y.dims() != x.dims()) {
      // Maybe need reduce here
      auto reduce_dim = get_reduce_dims(x.dims(), y.dims());
      if (!reduce_dim.size()) {
        by_pass<T>(out_grad, dx);
      } else {
        auto dx_reduce_res =
            out_grad.sum(common::vectorize(reduce_dim), x.dtype(), false);
        auto dx_tmp = reshape<T>(dx_reduce_res, common::vectorize(x.dims()));
        set_output<T>(dx_tmp, dx);
      }
    } else {
      by_pass<T>(out_grad, dx);
    }
  }
}

template <typename T>
void multiply_grad(const Tensor& x,
                   const Tensor& y,
                   const Tensor& out_grad,
                   int axis,
                   Tensor* x_grad,
                   Tensor* y_grad) {
  if (x_grad) {
    auto x_grad_unreduce = out_grad * y;
    if (x_grad_unreduce.dims() != x.dims()) {
      auto axes = get_reduce_dims_from_out(x_grad_unreduce.dims(), x.dims());
      if (!axes.size()) {
        set_output<T>(x_grad_unreduce, x_grad);
      } else {
        auto x_grad_reduced = x_grad_unreduce.sum(
            common::vectorize(axes), x_grad_unreduce.dtype(), false);
        if (x_grad_reduced.dims().size() != x.dims().size()) {
          x_grad_reduced = reshape<T>(x_grad_reduced, x.shape());
        }
        set_output<T>(x_grad_reduced, x_grad);
      }
    } else {
      set_output<T>(x_grad_unreduce, x_grad);
    }
  }
  if (y_grad) {
    auto y_grad_unreduce = out_grad * x;
    if (y_grad_unreduce.dims() != y.dims()) {
      auto axes = get_reduce_dims_from_out(y_grad_unreduce.dims(), y.dims());
      if (!axes.size()) {
        set_output<T>(y_grad_unreduce, y_grad);
      } else {
        auto y_grad_reduced = y_grad_unreduce.sum(
            common::vectorize(axes), y_grad_unreduce.dtype(), false);
        if (y_grad_reduced.dims().size() != y.dims().size()) {
          y_grad_reduced = reshape<T>(y_grad_reduced, y.shape());
        }
        set_output<T>(y_grad_reduced, y_grad);
      }
    } else {
      set_output<T>(y_grad_unreduce, y_grad);
    }
  }
}

template <typename T>
void elementwise_pow_grad(const Tensor& x,
                          const Tensor& y,
                          const Tensor& out_grad,
                          Tensor* dx,
                          Tensor* dy) {
  if (dy) {
    // dy = lnx * x^y
    auto lnx = log<T>(x);
    auto x_pow_y = elementwise_pow<T>(x, y);
    auto dy_res = lnx * x_pow_y * out_grad;
    if (x.dims() != y.dims()) {
      // Maybe need reduce here
      phi::DDim reduce_dim = get_reduce_dims(y.dims(), x.dims());
      if (!reduce_dim.size()) {
        set_output<T>(dy_res, dy);
      } else {
        auto dy_reduce_res =
            dy_res.sum(common::vectorize(reduce_dim), y.dtype(), false);
        auto dy_tmp = reshape<T>(dy_reduce_res, common::vectorize(y.dims()));
        set_output<T>(dy_tmp, dy);
      }
    } else {
      set_output<T>(dy_res, dy);
    }
  }  // indicate we will compute dy
  if (dx) {
    // dx = y * x^(y-1)
    auto tmp_z = y - 1.0;
    auto x_pow_z = elementwise_pow<T>(x, tmp_z);
    auto dx_res = y * x_pow_z * out_grad;
    if (y.dims() != x.dims()) {
      // Maybe need reduce here
      auto reduce_dim = get_reduce_dims(x.dims(), y.dims());
      if (!reduce_dim.size()) {
        set_output<T>(dx_res, dx);
      } else {
        auto dx_reduce_res =
            dx_res.sum(common::vectorize(reduce_dim), x.dtype(), false);
        auto dx_tmp = reshape<T>(dx_reduce_res, common::vectorize(x.dims()));
        set_output<T>(dx_tmp, dx);
      }

    } else {
      set_output<T>(dx_res, dx);
    }
  }  // indicate we will compute dx
}

template <typename T>
void layer_norm_grad(const Tensor& x,
                     const paddle::optional<Tensor>& scale,
                     const paddle::optional<Tensor>& bias,
                     const Tensor& mean,
                     const Tensor& variance,
                     const Tensor& out_grad,
                     float epsilon,
                     int begin_norm_axis,
                     Tensor* x_grad,
                     Tensor* scale_grad,
                     Tensor* bias_grad) {
  auto x_dims = x.dims();
  auto shape_1 = 1;  // front part
  auto shape_2 = 1;  // back part
  for (int i = 0; i < begin_norm_axis; ++i) {
    shape_1 *= x_dims[i];
  }
  for (int i = begin_norm_axis; i < x.dims().size(); ++i) {
    shape_2 *= x_dims[i];
  }
  auto scale_ptr = scale.get_ptr();
  auto bias_ptr = bias.get_ptr();

  auto x_cast = reshape<T>(x, std::vector<int64_t>({shape_1, shape_2}));
  auto out_grad_cast =
      reshape<T>(out_grad, std::vector<int64_t>({shape_1, shape_2}));
  auto mean_ = reshape<T>(mean, std::vector<int64_t>({shape_1, 1}));
  auto variance_ = reshape<T>(variance, std::vector<int64_t>({shape_1, 1}));

  Tensor scale_cast;
  if (scale_ptr) {
    scale_cast = reshape<T>(*scale_ptr, std::vector<int64_t>({1, shape_2}));
  }

  // cast dtype to float32 if dtype =float16 or bfloat16
  if (x.dtype() == phi::DataType::FLOAT16 ||
      x.dtype() == phi::DataType::BFLOAT16) {
    x_cast = cast<T>(x_cast, phi::DataType::FLOAT32);
    out_grad_cast = cast<T>(out_grad_cast, phi::DataType::FLOAT32);
    if (scale_ptr) {
      scale_cast = cast<T>(scale_cast, phi::DataType::FLOAT32);
    }
  }

  auto x_sub_mean = x_cast - mean_;          // M,N
  auto tmp = (1.0 / (variance_ + epsilon));  // M,1
  // auto sqrt_var_1 = sqrt<T>(tmp);            // M,1
  auto sqrt_var_1 = elementwise_pow<T>(
      tmp, full<T>(common::vectorize(tmp.dims()), 0.5, tmp.dtype()));
  auto x_sub_mean_mul_sqrt_var_1 = x_sub_mean * sqrt_var_1;

  if (x_grad) {
    auto out_grad_scale = out_grad_cast;  // M,N
    if (scale_ptr) {
      out_grad_scale = out_grad_cast * scale_cast;  // M,N * 1,N = M,N
    }

    auto dx_end = sqrt_var_1 * out_grad_scale;
    auto d_mean =
        dx_end.sum(std::vector<int64_t>({1}), x_cast.dtype(), true);  // M,1

    auto d_std_1 =
        (tmp * x_sub_mean * out_grad_scale)
            .sum(std::vector<int64_t>({1}), x_cast.dtype(), true);  // M,1
    auto d_std = d_std_1 * x_sub_mean_mul_sqrt_var_1;  // M,1 * M,N = M,N

    auto d_mean_d_std = (1.0 / shape_2) * (d_mean + d_std);
    auto x_grad_tmp = dx_end - d_mean_d_std;
    x_grad_tmp = reshape<T>(x_grad_tmp, common::vectorize(x.dims()));

    if (x.dtype() == phi::DataType::FLOAT16 ||
        x.dtype() == phi::DataType::BFLOAT16) {
      x_grad_tmp = cast<T>(x_grad_tmp, x.dtype());
    }
    set_output<T>(x_grad_tmp, x_grad);
  }

  if (scale_grad) {
    if (scale_ptr) {
      auto scale_grad_tmp =
          (x_sub_mean_mul_sqrt_var_1 * out_grad_cast)
              .sum(std::vector<int64_t>({0}), x_cast.dtype(), true);
      scale_grad_tmp = reshape<T>(scale_grad_tmp, scale_ptr->shape());
      if (scale_ptr->dtype() == phi::DataType::FLOAT16 ||
          scale_ptr->dtype() == phi::DataType::BFLOAT16) {
        scale_grad_tmp = cast<T>(scale_grad_tmp, scale_ptr->dtype());
      }
      set_output<T>(scale_grad_tmp, scale_grad);
    } else {
      scale_grad = nullptr;
    }
  }

  if (bias_grad) {
    if (bias_ptr) {
      auto bias_grad_tmp =
          out_grad_cast.sum(std::vector<int64_t>({0}), x_cast.dtype(), true);
      bias_grad_tmp = reshape<T>(bias_grad_tmp, bias_ptr->shape());
      if (bias_ptr->dtype() == phi::DataType::FLOAT16 ||
          bias_ptr->dtype() == phi::DataType::BFLOAT16) {
        bias_grad_tmp = cast<T>(bias_grad_tmp, bias_ptr->dtype());
      }
      set_output<T>(bias_grad_tmp, bias_grad);
    } else {
      bias_grad = nullptr;
    }
  }
}

template <typename T>
void dropout_grad(const Tensor& mask,
                  const Tensor& out_grad,
                  const Scalar& p,
                  bool is_test,
                  const std::string& mode,
                  Tensor* x_grad) {
  if (!x_grad) return;
  if (is_test) {
    if (mode == "upscale_in_train") {
      by_pass<T>(out_grad, x_grad);
    } else {
      set_output<T>(out_grad * (1.0 - p.to<float>()), x_grad);
    }
  } else {
    if (mode == "upscale_in_train") {
      if (p.to<float>() == 1.0f) {
        set_output<T>(scale<T>(out_grad, 0.0), x_grad);
      } else {
        set_output<T>(scale<T>(out_grad * cast<T>(mask, out_grad.dtype()),
                               1.0 / (1.0 - p.to<float>())),
                      x_grad);
      }
    } else {
      set_output<T>(out_grad * cast<T>(mask, out_grad.dtype()), x_grad);
    }
  }
}

template <typename T>
void erf_grad(const Tensor& x, const Tensor& out_grad, Tensor* x_grad) {
  if (x_grad) {
    auto m_2_sqrt_pi =
        full<T>(common::vectorize(x.dims()), M_2_SQRTPI, x.dtype());
    auto neg_one = full<T>(common::vectorize(x.dims()), -1.0, x.dtype());
    auto neg_tmp = neg_one * x * x;
    auto mul_tmp = m_2_sqrt_pi * exp<T>(neg_tmp);
    set_output<T>(out_grad * mul_tmp, x_grad);
  }
}

template <typename T>
void expand_grad(const Tensor& x,
                 const Tensor& out_grad,
                 const IntArray& shape,
                 Tensor* x_grad) {
  if (x_grad) {
    auto out_dims = common::make_ddim(shape.GetData());
    if (out_dims != x.dims()) {
      auto axes = get_reduce_dims(x.dims(), out_dims);
      if (!axes.size()) {
        by_pass<T>(out_grad, x_grad);
      } else {
        auto reduced = out_grad.sum(common::vectorize(axes), x.dtype(), false);
        if (reduced.dims().size() != x.dims().size()) {
          reduced = reshape<T>(reduced, x.shape());
        }
        set_output<T>(reduced, x_grad);
      }
    } else {
      by_pass<T>(out_grad, x_grad);
    }
  }
}

template <typename T>
void log_grad(const Tensor& x, const Tensor& out_grad, Tensor* x_grad) {
  if (x_grad) {
    // dx = dout / x
    set_output<T>(out_grad / x, x_grad);
  }
}

template <typename T>
void exp_grad(const Tensor& out, const Tensor& out_grad, Tensor* x_grad) {
  if (x_grad) {
    if (out.dtype() == phi::DataType::FLOAT16 ||
        out.dtype() == phi::DataType::BFLOAT16) {
      Tensor out_promote = cast<T>(out, phi::DataType::FLOAT32);
      Tensor out_grad_promote = cast<T>(out_grad, phi::DataType::FLOAT32);
      set_output<T>(cast<T>(out_promote * out_grad_promote, out.dtype()),
                    x_grad);
    } else {
      set_output<T>(out_grad * out, x_grad);
    }
  }
}

template <typename T>
void sqrt_grad(const Tensor& out, const Tensor& out_grad, Tensor* x_grad) {
  if (x_grad) {
    // This calculation is important for resnet.
    auto x_grad_tmp = (0.5 / out) * out_grad;
    set_output<T>(x_grad_tmp, x_grad);
  }
}

template <typename T>
void silu_grad(const Tensor& x,
               const Tensor& out,
               const Tensor& out_grad,
               Tensor* x_grad) {
  if (x_grad) {
    auto org_dtype = x.dtype();
    bool need_cast = org_dtype == phi::DataType::FLOAT16 ||
                     org_dtype == phi::DataType::BFLOAT16;
    if (need_cast) {
      auto x_cast = cast<T>(x, phi::DataType::FLOAT32);
      auto out_cast = cast<T>(out, phi::DataType::FLOAT32);
      auto out_grad_cast = cast<T>(out_grad, phi::DataType::FLOAT32);
      auto sigmoid = 1.0 / (1.0 + exp<T>(-x_cast));
      auto res = out_grad_cast * sigmoid * (1.0 + x_cast - out_cast);
      set_output<T>(cast<T>(res, org_dtype), x_grad);
    } else {
      auto sigmoid = 1.0 / (1.0 + exp<T>(-x));
      auto res = out_grad * sigmoid * (1.0 + x - out);
      set_output<T>(res, x_grad);
    }
  }
}

template <typename T>
void softmax_grad(const Tensor& out,
                  const Tensor& out_grad,
                  int axis,
                  Tensor* x_grad) {
  if (x_grad) {
    if (out_grad.dims().size() > 0) {
      if (axis >= 0) {
        auto new_out_grad = out_grad * out;
        auto tmp_x_grad = new_out_grad -
                          out * sum<T>(new_out_grad, {axis}, out.dtype(), true);
        set_output<T>(tmp_x_grad, x_grad);
      } else {
        auto new_out_grad = out_grad * out;
        auto tmp_x_grad =
            new_out_grad - out * sum<T>(new_out_grad,
                                        {out.dims().size() + axis},
                                        out.dtype(),
                                        true);
        set_output<T>(tmp_x_grad, x_grad);
      }
    } else {
      set_output<T>(out_grad * 0.0, x_grad);
    }
  }
}

template <typename T>
void maximum_grad(const Tensor& x,
                  const Tensor& y,
                  const Tensor& out_grad,
                  Tensor* x_grad,
                  Tensor* y_grad) {
  if (x_grad) {
    auto x_tmp = cast<T>(greater_than<T>(x, y), out_grad.dtype());
    auto dx_res = out_grad * x_tmp;
    if (y.dims() != x.dims()) {
      // Maybe need reduce here
      auto reduce_dim = get_reduce_dims(x.dims(), y.dims());
      if (!reduce_dim.size()) {
        set_output<T>(dx_res, x_grad);
      } else {
        auto dx_reduce_res =
            dx_res.sum(common::vectorize(reduce_dim), x.dtype(), false);
        auto dx_tmp = reshape<T>(dx_reduce_res, common::vectorize(x.dims()));
        set_output<T>(dx_tmp, x_grad);
      }
    } else {
      set_output<T>(dx_res, x_grad);
    }
  }

  if (y_grad) {
    auto y_tmp = cast<T>(less_equal<T>(x, y), out_grad.dtype());
    auto dy_res = out_grad * y_tmp;
    if (x.dims() != y.dims()) {
      // Maybe need reduce here
      phi::DDim reduce_dim = get_reduce_dims(y.dims(), x.dims());
      if (!reduce_dim.size()) {
        set_output<T>(dy_res, y_grad);
      } else {
        auto dy_reduce_res =
            dy_res.sum(common::vectorize(reduce_dim), y.dtype(), false);
        auto dy_tmp = reshape<T>(dy_reduce_res, common::vectorize(y.dims()));
        set_output<T>(dy_tmp, y_grad);
      }
    } else {
      set_output<T>(dy_res, y_grad);
    }
  }
}

template <typename T>
void relu_grad(const Tensor& out, const Tensor& out_grad, Tensor* x_grad) {
  if (x_grad) {
    auto condition = greater_than<T>(
        out, full<T>(common::vectorize(out.dims()), 0.0, out.dtype()));
    auto res =
        where<T>(condition,
                 out_grad,
                 full<T>(common::vectorize(out.dims()), 0.0, out.dtype()));
    set_output<T>(res, x_grad);
  }
}

template <typename T>
void gather_grad(const Tensor& x,
                 const Tensor& index,
                 const Tensor& out_grad,
                 const Scalar& axis,
                 Tensor* grad_x) {
  auto zero_tensor = full<T>(common::vectorize(x.dims()), 0.0, x.dtype());
  std::vector<int> tmp_perm;

  // change axis to rank 0
  int axis_value = axis.to<int>();
  tmp_perm.push_back(axis_value);
  // make other ranks
  for (int i = 0; i < x.dims().size(); ++i) {
    if (i != axis_value) {
      tmp_perm.push_back(i);
    }
  }
  std::vector<int> reverse_perm(tmp_perm);
  // make origin ranks
  for (int i = 0; i < static_cast<int>(tmp_perm.size()); ++i) {
    if (tmp_perm[i] >= 0) {
      reverse_perm[tmp_perm[i]] = i;
    } else {
      reverse_perm[tmp_perm[i] + tmp_perm.size()] = i;
    }
  }

  // transpose out_grad and zero grad to target rank.
  auto tmp_zero_x_grad = zero_tensor;
  auto tmp_out_grad = out_grad;
  if (zero_tensor.dims().size() > 0) {
    tmp_zero_x_grad = transpose<T>(zero_tensor, tmp_perm);
  }
  if (out_grad.dims().size() > 0) {
    tmp_out_grad = transpose<T>(out_grad, tmp_perm);
  }
  // scatter grad to grad_x
  auto tmp_grad_x = scatter<T>(tmp_zero_x_grad, index, tmp_out_grad, false);
  auto tmp_grad_x_tranposed = tmp_grad_x;
  if (tmp_grad_x.dims().size() > 0) {
    tmp_grad_x_tranposed = transpose<T>(tmp_grad_x, reverse_perm);
  }
  set_output<T>(tmp_grad_x_tranposed, grad_x);
}

template <typename T>
void gather_nd_grad(const Tensor& x,
                    const Tensor& index,
                    const Tensor& out_grad,
                    Tensor* x_grad) {
  if (x_grad) {
    auto zero_tensor = full<T>(common::vectorize(x.dims()), 0.0, x.dtype());
    auto x_grad_tmp = scatter_nd_add<T>(zero_tensor, index, out_grad);
    set_output<T>(x_grad_tmp, x_grad);
  }
}

template <typename T>
void instance_norm_grad(const Tensor& x,
                        const paddle::optional<Tensor>& scale,
                        const Tensor& saved_mean,
                        const Tensor& saved_variance,
                        const Tensor& y_grad,
                        float epsilon,
                        Tensor* x_grad,
                        Tensor* scale_grad,
                        Tensor* bias_grad) {
  const int n = x.dims()[0];
  const int c = x.dims()[1];
  const int h = x.dims()[2];
  const int w = x.dims()[3];

  auto promoted_y_grad = y_grad;
  if (x.dtype() == phi::DataType::FLOAT16 ||
      x.dtype() == phi::DataType::BFLOAT16) {
    promoted_y_grad = cast<T>(y_grad, phi::DataType::FLOAT32);
  }

  Tensor x_hat;
  Tensor std_inv;
  if (scale_grad || x_grad) {
    auto promoted_x = x;
    auto promoted_saved_mean = saved_mean;
    auto promoted_saved_var = saved_variance;
    if (x.dtype() == phi::DataType::FLOAT16 ||
        x.dtype() == phi::DataType::BFLOAT16) {
      promoted_x = cast<T>(x, phi::DataType::FLOAT32);
      promoted_saved_mean = cast<T>(saved_mean, phi::DataType::FLOAT32);
      promoted_saved_var = cast<T>(saved_variance, phi::DataType::FLOAT32);
    }
    auto mean = reshape<T>(promoted_saved_mean, IntArray({n, c, 1, 1}))
                    .tile(IntArray({1, 1, h, w}));
    std_inv = reshape<T>(promoted_saved_var, IntArray({n, c, 1, 1}))
                  .tile(IntArray({1, 1, h, w}));
    x_hat = (promoted_x - mean) * std_inv;
  }

  // x_grad = scale * inv_var * (y_grad - y_grad.mean(2,3) - x_hat * (y_grad *
  // x_hat).mean((h,w)))
  if (x_grad) {
    auto scale_data =
        reshape<T>(scale.get_ptr() ? scale.get()
                                   : full<T>(IntArray({c}), 1., x.dtype()),
                   IntArray({1, c, 1, 1}))
            .tile(IntArray({n, 1, h, w}));
    auto promoted_scale = scale_data;
    if (scale_data.dtype() == phi::DataType::FLOAT16 ||
        scale_data.dtype() == phi::DataType::BFLOAT16) {
      promoted_scale = cast<T>(scale_data, phi::DataType::FLOAT32);
    }
    auto result =
        (promoted_scale * std_inv) *
        (promoted_y_grad -
         promoted_y_grad.sum(IntArray({2, 3}), promoted_y_grad.dtype(), true) /
             (h * w) -
         (x_hat * ((promoted_y_grad * x_hat)
                       .sum(IntArray({2, 3}), promoted_y_grad.dtype(), true) /
                   (h * w))));
    if (x.dtype() == phi::DataType::FLOAT16 ||
        x.dtype() == phi::DataType::BFLOAT16) {
      set_output<T>(cast<T>(result, x.dtype()), x_grad);
    } else {
      set_output<T>(result, x_grad);
    }
  }
  // scale_grad = x_hat * y_grad.sum(n, h, w)
  if (scale_grad) {
    auto result = (promoted_y_grad * x_hat).sum(IntArray({0, 2, 3}));
    auto scale_dtype = scale.get_ptr() ? scale.get().dtype() : x.dtype();
    if (scale_dtype == phi::DataType::FLOAT16 ||
        scale_dtype == phi::DataType::BFLOAT16) {
      set_output<T>(cast<T>(result, scale_dtype), scale_grad);
    } else {
      set_output<T>(result, scale_grad);
    }
  }
  // d_bias = y_grad.sum(n, h, w)
  if (bias_grad) {
    auto result = promoted_y_grad.sum(IntArray({0, 2, 3}));
    auto scale_dtype = scale.get_ptr() ? scale.get().dtype() : x.dtype();
    if (scale_dtype == phi::DataType::FLOAT16 ||
        scale_dtype == phi::DataType::BFLOAT16) {
      set_output<T>(cast<T>(result, scale_dtype), bias_grad);
    } else {
      set_output<T>(result, bias_grad);
    }
  }
}

template <typename T>
void pad_grad(const Tensor& input,
              const Tensor& out_grad,
              const std::vector<int>& paddings,
              const Scalar& pad_value,
              Tensor* input_grad) {
  if (input_grad) {
    size_t rank = input.dims().size();
    auto out_dims = out_grad.dims();

    std::vector<int64_t> starts(rank, 0);
    std::vector<int64_t> ends(rank, 0);
    std::vector<int64_t> axes(rank, 0);
    std::vector<int64_t> infer_flags(rank, 1);
    std::vector<int64_t> decrease_axis({});
    for (size_t i = 0; i < rank; ++i) {
      starts[i] = static_cast<int64_t>(paddings[2 * i]);
      ends[i] = static_cast<int64_t>(out_dims[i] - paddings[2 * i + 1]);
      axes[i] = i;
    }
    auto out_tmp =
        slice<T>(out_grad, axes, starts, ends, infer_flags, decrease_axis);
    set_output<T>(out_tmp, input_grad);
  }
}

template <typename T>
void max_grad(const Tensor& x,
              const Tensor& out,
              const Tensor& out_grad,
              const IntArray& axis,
              bool keepdim,
              bool reduce_all,
              Tensor* x_grad) {
  if (!x_grad) {
    return;
  }
  auto zero_tensor = full<T>(common::vectorize(x.dims()), 0.0, x.dtype());
  std::vector<int64_t> x_dim = common::vectorize<int64_t>(x.dims());
  int64_t axis_size = axis.size();
  int64_t x_dim_size = x_dim.size();
  reduce_all = false;
  if (reduce_all || axis_size == 0 || axis_size == x_dim_size) {
    reduce_all = true;
  } else {
    reduce_all = false;
  }
  auto x_grad_tmp = Tensor();
  if (x_dim_size == 0 || x_dim_size == 1 || keepdim) {
    auto out_grad_tmp = out_grad.expand(IntArray(x_dim));
    auto out_tmp = out.expand(IntArray(x_dim));
    auto mask = equal<T>(x, out_tmp);
    x_grad_tmp = where<T>(mask, out_grad_tmp, zero_tensor);
  } else {
    auto axis_ = std::vector<int64_t>();
    if (reduce_all) {
      for (int64_t i = 0; i < x_dim_size; i++) {
        axis_.push_back(i);
      }
    } else {
      axis_ = axis.GetData();
      for (int64_t i = 0; i < axis_size; i++) {
        if (axis[i] < 0) {
          axis_[i] = axis[i] + x_dim_size;
        }
      }
    }
    auto out_grad_shape = get_unsqueeze_dims(out_grad, axis_);
    auto out_grad_ = reshape<T>(out_grad, out_grad_shape);
    auto out_ = reshape<T>(out, out_grad_shape);
    auto out_grad_tmp = out_grad_.expand(IntArray(x_dim));
    auto out_tmp = out_.expand(IntArray(x_dim));
    auto mask = equal<T>(x, out_tmp);
    x_grad_tmp = where<T>(mask, out_grad_tmp, zero_tensor);
  }
  set_output<T>(x_grad_tmp, x_grad);
}

template <typename T>
void slice_grad(const Tensor& input,
                const Tensor& out_grad,
                const std::vector<int64_t>& axes,
                const IntArray& starts,
                const IntArray& ends,
                const std::vector<int64_t>& infer_flags,
                const std::vector<int64_t>& decrease_axis,
                Tensor* input_grad) {
  if (input_grad) {
    size_t rank = input.dims().size();
    auto out_dims = out_grad.dims();
    std::vector<int64_t> origin_out_shape;
    auto in_dims = input.dims();

    auto decrease_size = decrease_axis.size();
    if (decrease_size > 0) {
      if (decrease_size == static_cast<size_t>(in_dims.size())) {
        // all dims decrease
        out_dims = common::make_ddim(std::vector<int>(decrease_size, 1));
      } else {
        origin_out_shape.resize(out_dims.size() + decrease_size, -1);
        for (size_t i = 0; i < decrease_size; ++i) {
          origin_out_shape[decrease_axis[i]] = 1;
        }

        int index = 0;
        for (size_t i = 0; i < origin_out_shape.size(); ++i) {
          if (origin_out_shape[i] == -1) {
            origin_out_shape[i] = out_dims[index];
            ++index;
          }
        }
        out_dims = common::make_ddim(origin_out_shape);
      }
    }

    std::vector<int> offsets(rank, 0);
    std::vector<int> extents(rank, 0);
    for (size_t i = 0; i < rank; ++i) {
      offsets[i] = 0;
      extents[i] = out_dims[i];
    }
    for (size_t i = 0; i < axes.size(); ++i) {
      int axis = axes[i];
      int64_t start = starts[i] < 0 ? (starts[i] + in_dims[axis]) : starts[i];
      start = std::max(start, static_cast<int64_t>(0));
      offsets[axis] = start;
    }

    std::vector<int> paddings;
    for (size_t i = 0; i < rank; ++i) {
      paddings.push_back(offsets[i]);
      paddings.push_back((in_dims[i] - out_dims[i]) - offsets[i]);
    }
    if (decrease_size > 0 &&
        (decrease_size != static_cast<size_t>(in_dims.size()))) {
      auto out_tmp =
          pad<T>(reshape<T>(out_grad, origin_out_shape), paddings, 0.0);
      set_output<T>(out_tmp, input_grad);
    } else {
      auto out_tmp = pad<T>(out_grad, paddings, 0.0);
      set_output<T>(out_tmp, input_grad);
    }
  }
}

template <typename T>
void tile_grad(const Tensor& x,
               const Tensor& out_grad,
               const IntArray& repeat_times,
               Tensor* x_grad) {
  if (x_grad) {
    auto repeat_times_data = repeat_times.GetData();
    auto out_grad_shape = common::vectorize<int>(out_grad.dims());
    auto result = out_grad;
    for (int i = 0; i < static_cast<int>(repeat_times_data.size()); i++) {
      int size = out_grad_shape[i] / repeat_times_data[i];
      std::vector<int> sections(repeat_times_data[i], size);
      auto split_arr = split<T>(result, IntArray(sections), i);
      result = full<T>(common::vectorize(split_arr[0].dims()), 0.0, x.dtype());
      for (int j = 0; j < static_cast<int>(split_arr.size()); j++) {
        result = split_arr[j] + result;
      }
    }
    result = reshape<T>(result, x.shape());
    set_output<T>(result, x_grad);
  }
}

template <typename T>
void hardswish_grad(const Tensor& x, const Tensor& out_grad, Tensor* x_grad) {
  if (x_grad) {
    auto offset = full<T>(common::vectorize(x.dims()), 3.0, x.dtype());
    auto condition = less_equal<T>(x, offset);
    auto tmp1 = where<T>(condition, out_grad * ((x / 3.0) + 0.5), out_grad);
    auto res = where<T>(
        less_than<T>(x, full<T>(common::vectorize(x.dims()), -3.0, x.dtype())),
        full<T>(common::vectorize(x.dims()), 0.0, x.dtype()),
        tmp1);
    set_output<T>(res, x_grad);
  }
}

template <typename T>
void leaky_relu_grad(const Tensor& out,
                     const Tensor& out_grad,
                     float negative_slope,
                     Tensor* x_grad) {
  if (x_grad) {
    auto condition = greater_than<T>(
        out, full<T>(common::vectorize(out.dims()), 0.0, out.dtype()));
    auto res = where<T>(condition, out_grad, out_grad * negative_slope);
    set_output<T>(res, x_grad);
  }
}

template <typename T>
void sigmoid_grad(const Tensor& out, const Tensor& out_grad, Tensor* x_grad) {
  if (x_grad) {
    set_output<T>(out_grad * (out * (1 - out)), x_grad);
  }
}

template <typename T>
void topk_grad(const Tensor& x,
               const Tensor& indices,
               const Tensor& out_grad,
               const Scalar& k,
               const int& axis,
               const bool& largest,
               const bool& sorted,
               Tensor* x_grad) {
  if (x_grad) {
    // put_along_axis doesn't support zero dim
    if (x.dims().size() == 0) {
      by_pass<T>(out_grad, x_grad);
      return;
    }
    auto zero_tensor = full<T>(common::vectorize(x.dims()), 0, x.dtype());
    auto x_grad_tmp = put_along_axis<T>(zero_tensor, indices, out_grad, axis);
    set_output<T>(x_grad_tmp, x_grad);
  }
}

template <typename T>
void prod_grad(const Tensor& x,
               const Tensor& out,
               const Tensor& out_grad,
               const IntArray& axis,
               bool keep_dim,
               bool reduce_all,
               Tensor* x_grad) {
  if (x_grad) {
    std::vector<int64_t> x_dim = common::vectorize<int64_t>(x.dims());
    int64_t axis_size = axis.size();
    int64_t x_dim_size = x_dim.size();
    reduce_all = false;
    if (reduce_all || axis_size == 0 || axis_size == x_dim_size) {
      reduce_all = true;
    } else {
      reduce_all = false;
    }
    auto x_grad_tmp = Tensor();
    auto out_tmp = Tensor();
    if (x_dim_size == 1) {
      x_grad_tmp = out_grad.expand(IntArray(x_dim));
      out_tmp = out.expand(IntArray(x_dim));
    } else {
      if (!keep_dim) {
        auto axis_ = std::vector<int64_t>();
        if (reduce_all) {
          for (int64_t i = 0; i < x_dim_size; i++) {
            axis_.push_back(i);
          }
        } else {
          axis_ = axis.GetData();
          for (int64_t i = 0; i < axis_size; i++) {
            if (axis[i] < 0) {
              axis_[i] = axis[i] + x_dim_size;
            }
          }
        }
        auto out_grad_shape = get_unsqueeze_dims(out_grad, axis_);
        auto out_grad_ = reshape<T>(out_grad, out_grad_shape);
        x_grad_tmp = out_grad_.expand(IntArray(x_dim));
        auto out_ = reshape<T>(out, out_grad_shape);
        out_tmp = out_.expand(IntArray(x_dim));
      } else {
        x_grad_tmp = out_grad.expand(IntArray(x_dim));
        out_tmp = out.expand(IntArray(x_dim));
      }
    }
    auto x_grad_res = x_grad_tmp * out_tmp * (1 / x);
    set_output<T>(x_grad_res, x_grad);
  }
}

template <typename T>
void minimum_grad(const Tensor& x,
                  const Tensor& y,
                  const Tensor& out_grad,
                  Tensor* x_grad,
                  Tensor* y_grad) {
  if (x_grad) {
    auto x_tmp = cast<T>(less_than<T>(x, y), out_grad.dtype());
    auto dx_res = out_grad * x_tmp;
    if (y.dims() != x.dims()) {
      // Maybe need reduce here
      auto reduce_dim = get_reduce_dims(x.dims(), y.dims());
      if (!reduce_dim.size()) {
        set_output<T>(dx_res, x_grad);
      } else {
        auto dx_reduce_res =
            dx_res.sum(common::vectorize(reduce_dim), x.dtype(), false);
        auto dx_tmp = reshape<T>(dx_reduce_res, common::vectorize(x.dims()));
        set_output<T>(dx_tmp, x_grad);
      }
    } else {
      set_output<T>(dx_res, x_grad);
    }
  }

  if (y_grad) {
    auto y_tmp = cast<T>(greater_equal<T>(x, y), out_grad.dtype());
    auto dy_res = out_grad * y_tmp;
    if (x.dims() != y.dims()) {
      // Maybe need reduce here
      phi::DDim reduce_dim = get_reduce_dims(y.dims(), x.dims());
      if (!reduce_dim.size()) {
        set_output<T>(dy_res, y_grad);
      } else {
        auto dy_reduce_res =
            dy_res.sum(common::vectorize(reduce_dim), y.dtype(), false);
        auto dy_tmp = reshape<T>(dy_reduce_res, common::vectorize(y.dims()));
        set_output<T>(dy_tmp, y_grad);
      }
    } else {
      set_output<T>(dy_res, y_grad);
    }
  }
}

}  // namespace details
}  // namespace primitive
}  // namespace paddle