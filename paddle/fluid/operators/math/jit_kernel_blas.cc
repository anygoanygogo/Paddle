/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/operators/math/jit_kernel.h"
#include <string>
#include "paddle/fluid/operators/math/jit_kernel_macro.h"
#include "paddle/fluid/platform/enforce.h"

#ifdef PADDLE_WITH_XBYAK
#include "paddle/fluid/operators/math/jit_code.h"
#endif

#ifdef PADDLE_WITH_MKLML
#include "paddle/fluid/platform/dynload/mklml.h"
#endif

#ifdef __AVX__
#include <immintrin.h>
#endif

namespace paddle {
namespace operators {
namespace math {
namespace jitkernel {
namespace jit = platform::jit;

template <typename T>
void VMulRefer(const T* x, const T* y, T* z, int n) {
  for (int i = 0; i < n; ++i) {
    z[i] = x[i] * y[i];
  }
}

template <typename T>
void VAddRefer(const T* x, const T* y, T* z, int n) {
  for (int i = 0; i < n; ++i) {
    z[i] = x[i] + y[i];
  }
}

template <typename T>
void VAddReluRefer(const T* x, const T* y, T* z, int n) {
  for (int i = 0; i < n; ++i) {
    z[i] = x[i] + y[i];
    z[i] = z[i] > 0 ? z[i] : 0;
  }
}

template <typename T>
void VScalRefer(const T* a, const T* x, T* y, int n) {
  for (int i = 0; i < n; ++i) {
    y[i] = a[0] * x[i];
  }
}

template <typename T>
void VAddBiasRefer(const T* a, const T* x, T* y, int n) {
  for (int i = 0; i < n; ++i) {
    y[i] = a[0] + x[i];
  }
}

template <typename T>
void VReluRefer(const T* x, T* y, int n) {
  for (int i = 0; i < n; ++i) {
    y[i] = x[i] > 0 ? x[i] : 0;
  }
}

#ifdef PADDLE_WITH_MKLML
template <typename T>
void VMulMKL(const T* x, const T* y, T* z, int n);

template <>
void VMulMKL<float>(const float* x, const float* y, float* z, int n) {
  platform::dynload::vsMul(n, x, y, z);
}

template <>
void VMulMKL<double>(const double* x, const double* y, double* z, int n) {
  platform::dynload::vdMul(n, x, y, z);
}

template <typename T>
void VAddMKL(const T* x, const T* y, T* z, int n);

template <>
void VAddMKL<float>(const float* x, const float* y, float* z, int n) {
  platform::dynload::vsAdd(n, x, y, z);
}

template <>
void VAddMKL<double>(const double* x, const double* y, double* z, int n) {
  platform::dynload::vdAdd(n, x, y, z);
}

template <typename T>
void VScalMKL(const T* a, const T* x, T* y, int n);

template <>
void VScalMKL<float>(const float* a, const float* x, float* y, int n) {
  if (x == y) {
    platform::dynload::cblas_sscal(n, *a, y, 1);
  } else {
    VScalRefer<float>(a, x, y, n);
  }
}

template <>
void VScalMKL<double>(const double* a, const double* x, double* y, int n) {
  if (x == y) {
    platform::dynload::cblas_dscal(n, *a, y, 1);
  } else {
    VScalRefer<double>(a, x, y, n);
  }
}

#endif

#define DECLARE_STATIC_FUNC                                 \
  static inline std::string name(int d) {                   \
    PADDLE_THROW("DType should be either float or double"); \
  }                                                         \
  static inline bool useJIT(int d) { return false; }        \
  static inline bool useMKL(int d) { return false; }

/* VMUL JitKernel */
template <typename T>
class VMulKernelImpl : public VMulKernel<T> {
 public:
  DECLARE_STATIC_FUNC;
  explicit VMulKernelImpl(int d) : VMulKernel<T>() {
#ifdef PADDLE_WITH_XBYAK
    if (useJIT(d)) {
      // roughly estimate the size of code
      size_t sz = 96 + d / AVX_FLOAT_BLOCK * 4 * 8;
      jitcode_.reset(new gen::VXXJitCode(d, gen::operand_type::mul, 0, false,
                                         sz > 4096 ? sz : 4096));
      this->Compute =
          jitcode_->getCode<void (*)(const T*, const T*, T*, int)>();
      return;
    }
#endif
#ifdef PADDLE_WITH_MKLML
    if (useMKL(d)) {
      this->Compute = VMulMKL<T>;
      return;
    }
#endif
    this->Compute = VMulRefer<T>;
  }

#ifdef PADDLE_WITH_XBYAK

 private:
  std::unique_ptr<gen::VXXJitCode> jitcode_{nullptr};
#endif
};

#ifdef PADDLE_WITH_XBYAK
template <>
bool VMulKernelImpl<float>::useJIT(int d) {
  return gen::VXXJitCode::init(d);
}
#endif

#ifdef PADDLE_WITH_MKLML
template <>
bool VMulKernelImpl<float>::useMKL(int d) {
  return jit::MayIUse(jit::avx512f) && d > 512;
}

template <>
bool VMulKernelImpl<double>::useMKL(int d) {
  return true;
}
#endif

/* VAdd JitKernel */
template <typename T>
class VAddKernelImpl : public VAddKernel<T> {
 public:
  DECLARE_STATIC_FUNC;
  explicit VAddKernelImpl(int d) : VAddKernel<T>() {
#ifdef PADDLE_WITH_XBYAK
    if (useJIT(d)) {
      size_t sz = 96 + d / AVX_FLOAT_BLOCK * 4 * 8;
      jitcode_.reset(new gen::VXXJitCode(d, gen::operand_type::add, 0, false,
                                         sz > 4096 ? sz : 4096));
      this->Compute =
          jitcode_->getCode<void (*)(const T*, const T*, T*, int)>();
      return;
    }
#endif
#ifdef PADDLE_WITH_MKLML
    if (useMKL(d)) {
      this->Compute = VAddMKL<T>;
      return;
    }
#endif
    this->Compute = VAddRefer<T>;
  }
#ifdef PADDLE_WITH_XBYAK

 private:
  std::unique_ptr<gen::VXXJitCode> jitcode_{nullptr};
#endif
};

#ifdef PADDLE_WITH_XBYAK
template <>
bool VAddKernelImpl<float>::useJIT(int d) {
  return gen::VXXJitCode::init(d);
}
#endif

#ifdef PADDLE_WITH_MKLML
template <>
bool VAddKernelImpl<float>::useMKL(int d) {
  return d > 512;
}

template <>
bool VAddKernelImpl<double>::useMKL(int d) {
  return true;
}
#endif

/* VAddRelu JitKernel */
template <typename T>
class VAddReluKernelImpl : public VAddReluKernel<T> {
 public:
  DECLARE_STATIC_FUNC;
  explicit VAddReluKernelImpl(int d) : VAddReluKernel<T>() {
#ifdef PADDLE_WITH_XBYAK
    if (useJIT(d)) {
      size_t sz = 96 + d / AVX_FLOAT_BLOCK * 4 * 8;
      jitcode_.reset(new gen::VXXJitCode(d, gen::operand_type::add, 0, true,
                                         sz > 4096 ? sz : 4096));
      this->Compute =
          jitcode_->getCode<void (*)(const T*, const T*, T*, int)>();
      return;
    }
#endif
    this->Compute = VAddReluRefer<T>;
  }
#ifdef PADDLE_WITH_XBYAK

 private:
  std::unique_ptr<gen::VXXJitCode> jitcode_{nullptr};
#endif
};

#ifdef PADDLE_WITH_XBYAK
template <>
bool VAddReluKernelImpl<float>::useJIT(int d) {
  return gen::VXXJitCode::init(d);
}
#endif

/* VScal JitKernel */
template <typename T>
class VScalKernelImpl : public VScalKernel<T> {
 public:
  DECLARE_STATIC_FUNC;
  explicit VScalKernelImpl(int d) : VScalKernel<T>() {
#ifdef PADDLE_WITH_XBYAK
    if (useJIT(d)) {
      size_t sz = 96 + d / AVX_FLOAT_BLOCK * 4 * 8;
      jitcode_.reset(new gen::VXXJitCode(d, gen::operand_type::mul, 1, false,
                                         sz > 4096 ? sz : 4096));
      this->Compute =
          jitcode_->getCode<void (*)(const T*, const T*, T*, int)>();
      return;
    }
#endif
#ifdef PADDLE_WITH_MKLML
    if (useMKL(d)) {
      this->Compute = VScalMKL<T>;
      return;
    }
#endif
    this->Compute = VScalRefer<T>;
  }
#ifdef PADDLE_WITH_XBYAK

 private:
  std::unique_ptr<gen::VXXJitCode> jitcode_{nullptr};
#endif
};

#ifdef PADDLE_WITH_XBYAK
template <>
bool VScalKernelImpl<float>::useJIT(int d) {
  return gen::VXXJitCode::init(d, 1);
}
#endif

#ifdef PADDLE_WITH_MKLML
template <>
bool VScalKernelImpl<float>::useMKL(int d) {
  return d > 512;
}
template <>
bool VScalKernelImpl<double>::useMKL(int d) {
  return true;
}
#endif

/* VAddBias JitKernel */
template <typename T>
class VAddBiasKernelImpl : public VAddBiasKernel<T> {
 public:
  DECLARE_STATIC_FUNC;
  explicit VAddBiasKernelImpl(int d) : VAddBiasKernel<T>() {
#ifdef PADDLE_WITH_XBYAK
    if (useJIT(d)) {
      size_t sz = 96 + d / AVX_FLOAT_BLOCK * 4 * 8;
      jitcode_.reset(new gen::VXXJitCode(d, gen::operand_type::add, 1, false,
                                         sz > 4096 ? sz : 4096));
      this->Compute =
          jitcode_->getCode<void (*)(const T*, const T*, T*, int)>();
      return;
    }
#endif

    this->Compute = VAddBiasRefer<T>;
  }
#ifdef PADDLE_WITH_XBYAK

 private:
  std::unique_ptr<gen::VXXJitCode> jitcode_{nullptr};
#endif
};

#ifdef PADDLE_WITH_XBYAK
template <>
bool VAddBiasKernelImpl<float>::useJIT(int d) {
  return gen::VXXJitCode::init(d, 1);
}
#endif

/* VRelu JitKernel */
template <typename T>
class VReluKernelImpl : public VReluKernel<T> {
 public:
  DECLARE_STATIC_FUNC;
  explicit VReluKernelImpl(int d) : VReluKernel<T>() {
    this->num_ = d;  // TODO(TJ): remove me when ComputeDeprecated done
#ifdef PADDLE_WITH_XBYAK
    if (useJIT(d)) {
      size_t sz = 96 /*init*/ +
                  d / AVX_FLOAT_BLOCK * 4 /* instructions*/ *
                      8 /*everage byte for each instruction*/;
      jitcode_.reset(new gen::ReluJitCode(d, sz > 4096 ? sz : 4096));
      this->Compute = jitcode_->getCode<void (*)(const T*, T*, int)>();
      return;
    }
#endif

    this->Compute = VReluRefer<T>;
  }
  void ComputeDeprecated(const T* x, T* y) const override {
    VReluRefer(x, y, this->num_);
  }
#ifdef PADDLE_WITH_XBYAK

 private:
  std::unique_ptr<gen::ReluJitCode> jitcode_{nullptr};
#endif
};

#ifdef PADDLE_WITH_XBYAK
template <>
bool VReluKernelImpl<float>::useJIT(int d) {
  return gen::ReluJitCode::init(d);
}
#endif

#undef DECLARE_STATIC_FUNC

REGISTER_JITKERNEL(vmul, VMulKernel);
REGISTER_JITKERNEL(vadd, VAddKernel);
REGISTER_JITKERNEL(vaddrelu, VAddReluKernel);
REGISTER_JITKERNEL(vscal, VScalKernel);
REGISTER_JITKERNEL(vaddbias, VAddBiasKernel);
REGISTER_JITKERNEL(vrelu, VReluKernel);

/* An empty JitKernel */
template <typename T, platform::jit::cpu_isa_t isa, jit_block>
class VIdentityKernelImpl : public VIdentityKernel<T> {
 public:
  explicit VIdentityKernelImpl(int d) : VIdentityKernel<T>() { this->num_ = d; }
  void ComputeDeprecated(const T* x, T* y) const override {}
};

REGISTER_JITKERNEL_DEPRECATED(videntity, VIdentityKernel);

}  // namespace jitkernel
}  // namespace math
}  // namespace operators
}  // namespace paddle
