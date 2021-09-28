/*************************************************************************
 * Copyright (C) 2021 Cambricon.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *************************************************************************/
#include "pytorch_mlu_helper.hpp"
#include <string>
#include <vector>

// policy function
static void policyFunc(cnrtDim3_t *k_dim,
                       cnrtFunctionType_t *k_type,
                       const Tensor& input,
                       const Tensor& target,
                       const Tensor& weight) {
  auto N = input.size(0);
  auto C = input.size(1);

  auto nram_size = torch_mlu::getDeviceAttr(cnrtAttrNramSizePerMcore);
  auto c_align_size = PAD_UP((C * input.itemsize()), NFU_ALIGN_SIZE);
  const int split_target_num = 2;
  const int split_pipeline_num = 6;
  auto scalar_size = NFU_ALIGN_SIZE;
  auto weight_size = c_align_size;

  // n_seg * c_align_size * split_pipeline_num + n_seg * target.itemsize() * split_target_num
  //     + weight_size + scalar_size <= nram_size
  auto n_seg = (nram_size - weight_size - scalar_size) / (c_align_size * split_pipeline_num
                   + target.itemsize() * split_target_num);
  auto seg_num = (N + n_seg - 1) / n_seg;

  auto core_dim = torch_mlu::getDeviceAttr(cnrtAttrMcorePerCluster);
  auto cluster_num = torch_mlu::getDeviceAttr(cnrtAttrClusterCount);
  auto core_num = core_dim * cluster_num;

  k_dim->x = *k_type;
  k_dim->y = seg_num > core_num ? cluster_num : (seg_num + core_dim - 1) / core_dim;
  k_dim->z = 1;
}

void checkFocalLossSigmoidForwardValidation(const Tensor& input,
                                            const Tensor& target,
                                            const Tensor& weight,
                                            const Tensor& output) {
  // check shape
  TORCH_CHECK(input.dim() == 2, "Dimension num of input should be 2. ",  
              "But now is ", input.dim(), ".");

  TORCH_CHECK(target.dim() == 1, "Dimension num of target should be 1. ",  
              "But now is ", target.dim(), ".");

  TORCH_CHECK(input.size(0) == target.size(0), "Element num of target should be ", 
              input.size(0), ". But now is ", target.size(0), ".");

  TORCH_CHECK(output.dim() == 2, "Dimension num of target should be 2. ",
              "But now is ", output.dim(), ".");

  TORCH_CHECK(input.size(0) == output.size(0) && input.size(1) == output.size(1),
              "Shape of output and input must be euqal, but now output is ", 
              output.size(0), ", ", output.size(1), " and input is ",
              input.size(0), ", ", input.size(1), ".");

  // check dtype
  TORCH_CHECK(input.scalar_type() == at::kFloat || input.scalar_type() == at::kHalf, 
              "Data type of input should be Float or Half. But now input type is ",
              input.scalar_type(), ".");

  TORCH_CHECK(target.scalar_type() == at::kLong, "target type should be Long. ",
              "But now target type is ", target.scalar_type(), ".");

  TORCH_CHECK(output.scalar_type() == input.scalar_type(), 
              "Data types of input and output should be the same. But now input type is ",
              input.scalar_type(), ", output type is ", output.scalar_type(), ".");

  // check weight
  if (weight.data_ptr() != nullptr) {
    TORCH_CHECK(weight.scalar_type() == input.scalar_type(), 
                "Data types of input and weight should be the same. But now input type is ",
                input.scalar_type(), ", weight type is ", weight.scalar_type(), ".");

    TORCH_CHECK(weight.dim() == 1, "Dimension num of weight should be 1.",
                "But now is ", weight.dim(), ".");

    TORCH_CHECK(weight.size(0) == input.size(1), "Element num of weight should be ", 
                input.size(1), ". But now is ", weight.size(0), ".");
  } else {
    CNLOG(INFO) << "weight is a empty tensor.";
  }
}

void SigmoidFocalLossForwardMLUKernelLauncher(Tensor input,
                                              Tensor target,
                                              Tensor weight,
                                              Tensor output,
                                              float gamma,
                                              float alpha) {

  // params check
  TORCH_CHECK(gamma >= 0, "gamma should be greater than or equal to 0. ",
                          "But now gamma is ", gamma, ".");

  checkFocalLossSigmoidForwardValidation(input, target, weight, output);

  // check C
  auto input_N = input.size(0);
  auto input_C = input.size(1);
  auto split_target_num = 2;
  int split_pipeline_num = 6;
  auto nram_size = torch_mlu::getDeviceAttr(cnrtAttrNramSizePerMcore);

  // target supports only INT on MLU device
  // while it keeps LONG on host side, so target.itemsize()/2
  auto threshold_C = PAD_DOWN((nram_size - NFU_ALIGN_SIZE -
                               split_target_num * target.itemsize()/2) /
                               split_pipeline_num, NFU_ALIGN_SIZE) /
                               input.itemsize();

  TORCH_CHECK(threshold_C >= input_C, "input.size(1) should be in the range of [0, ",
          threshold_C, "]. ", "But now input.size(1) is ", input_C, ".");

  if (input.numel() == 0 || target.numel() == 0 || output.numel() == 0) {
    // return if zero-element
    return ;
  }

  // calculate task dimension
  cnrtDim3_t k_dim;
  cnrtFunctionType_t k_type = CNRT_FUNC_TYPE_UNION1;
  policyFunc(&k_dim, &k_type, input, target, weight);
  auto core_dim = torch_mlu::getDeviceAttr(cnrtAttrMcorePerCluster);

  // get compute queue
  auto queue = torch_mlu::getCurQueue();

  // get ptr of tensors
  auto input_impl = torch_mlu::getMluTensorImpl(input);
  auto input_ptr = input_impl->cnnlMalloc();
  auto target_impl = torch_mlu::getMluTensorImpl(target);
  auto target_ptr = target_impl->cnnlMalloc();
  auto weight_impl = torch_mlu::getMluTensorImpl(weight);
  auto weight_ptr = weight_impl->cnnlMalloc();
  auto output_impl = torch_mlu::getMluTensorImpl(output);
  auto output_ptr = output_impl->cnnlMalloc();

  // get dtype of input
  cnrtDataType_t d_type = torch_mlu::toCnrtDtype(input.dtype());

  CNLOG(INFO) << "Launch Kernel KernelFocalLossSigmoidForward<<<Union" << k_type / core_dim
          << ", " << k_dim.x << ", " << k_dim.y << ", " << k_dim.z << ">>>";
  // launch kernel
  KernelFocalLossSigmoidForward(k_dim, k_type, queue, d_type, input_ptr,
          target_ptr, weight_ptr, input_N, input_C, alpha, gamma, output_ptr);
}
