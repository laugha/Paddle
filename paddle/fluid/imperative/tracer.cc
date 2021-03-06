// Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
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

#include "paddle/fluid/imperative/tracer.h"

#include <set>

#include "paddle/fluid/operators/math/math_function.h"
#include "paddle/fluid/platform/device_context.h"
#include "paddle/fluid/platform/enforce.h"

namespace paddle {
namespace imperative {

void CreateGradOp(const framework::OpDesc& op_desc,
                  const std::unordered_set<std::string>& no_grad_set,
                  const std::vector<framework::BlockDesc*>& grad_sub_block,
                  std::vector<framework::OpDesc*>* grad_op_descs,
                  std::unordered_map<std::string, std::string>* grad_to_var) {
  PADDLE_ENFORCE(grad_op_descs->empty());
  std::vector<std::unique_ptr<framework::OpDesc>> descs =
      framework::OpInfoMap::Instance()
          .Get(op_desc.Type())
          .GradOpMaker()(op_desc, no_grad_set, grad_to_var, grad_sub_block);

  for (auto& desc : descs) {
    grad_op_descs->emplace_back(desc.release());
  }
}

void InitVar(framework::Variable* var, framework::Variable* grad_var,
             platform::DeviceContext* dev_ctx) {
  PADDLE_ENFORCE_NOT_NULL(dev_ctx,
                          "Could not get valid device from forward op");
  auto& var_t = var->Get<framework::LoDTensor>();
  grad_var->GetMutable<framework::LoDTensor>()->mutable_data<float>(
      var_t.dims(), dev_ctx->GetPlace());
  operators::math::set_constant(
      *dev_ctx, grad_var->GetMutable<framework::LoDTensor>(), 0.0);
}

platform::Place GetExpectedPlace(platform::Place place, VarBasePtrMap inputs) {
  platform::Place result = place;
  for (auto it : inputs) {
    for (VarBase* var : it.second) {
      platform::Place tmp_place =
          var->var_->Get<framework::LoDTensor>().place();
      if (!platform::is_same_place(tmp_place, result)) {
        PADDLE_THROW(
            "Input variable should keep in the same place: %s, but get place: "
            "%s of input %s instead",
            result, tmp_place, it.first);
      }
    }
  }

  return result;
}

std::set<std::string> Tracer::Trace(OpBase* op, const VarBasePtrMap& inputs,
                                    const VarBasePtrMap& outputs,
                                    framework::BlockDesc* block,
                                    const platform::Place expected_place,
                                    const bool stop_gradient) {
  std::map<std::string, VarBase*> vars;

  framework::OpDesc* op_desc = op->op_desc_;
  VLOG(3) << "tracer tracing " << op_desc->Type();
  op_desc->InferShape(*block);
  op_desc->InferVarType(block);

  std::unique_ptr<framework::OperatorBase> op_base =
      framework::OpRegistry::CreateOp(*op_desc);

  framework::VariableValueMap invars_map;
  framework::VariableValueMap outvars_map;

  op->input_vars_ = inputs;
  for (auto it : op->input_vars_) {
    auto& invars = invars_map[it.first];
    invars.reserve(it.second.size());
    for (VarBase* inp : it.second) {
      PADDLE_ENFORCE_NOT_NULL(inp->var_, "op %s input %s nullptr",
                              op->op_desc_->Type(), inp->var_desc_->Name());

      invars.emplace_back(inp->var_);
      vars[inp->var_desc_->Name()] = inp;
      if (inp->PreOp() && !inp->IsStopGradient()) {
        op->pre_ops_[it.first].push_back(inp->PreOp());
        op->pre_ops_out_idx_[it.first].push_back(inp->PreOpOutIdx());
      } else {
        op->pre_ops_[it.first].push_back(nullptr);
      }
      VLOG(3) << "input vname " << inp->var_desc_->Name() << " "
              << inp->var_->IsInitialized();
    }
  }

  op->output_vars_ = outputs;
  for (auto it : op->output_vars_) {
    auto& outvars = outvars_map[it.first];
    const std::vector<VarBase*>& outputs = it.second;
    outvars.reserve(outputs.size());
    for (size_t i = 0; i < outputs.size(); ++i) {
      VarBase* out = outputs[i];
      outvars.emplace_back(out->var_);
      vars[out->var_desc_->Name()] = out;

      framework::VarDesc* var_desc = block->FindVar(out->var_desc_->Name());
      if (var_desc->GetType() == framework::proto::VarType::LOD_TENSOR) {
        out->var_->GetMutable<framework::LoDTensor>();
      } else {
        LOG(ERROR) << "tracer doesn't support yet";
      }
      out->TrackPreOp(op, it.first, i, stop_gradient);

      VLOG(3) << "output vname " << out->var_desc_->Name() << " "
              << out->var_->IsInitialized();
    }
  }

  VLOG(3) << "tracer running " << op_desc->Type();
  framework::RuntimeContext ctx(invars_map, outvars_map);

  // TODO(panyx0718): Cache p.
  framework::OperatorWithKernel* op_kernel =
      dynamic_cast<framework::OperatorWithKernel*>(op_base.get());
  PADDLE_ENFORCE_NOT_NULL(op_kernel, "only support op with kernel");

  framework::Scope scope;
  op->place_ = GetExpectedPlace(expected_place, inputs);
  PreparedOp prepared_op = PreparedOp::Prepare(ctx, *op_kernel, op->place_);
  prepared_op.op.RuntimeInferShape(scope, op->place_, ctx);
  prepared_op.func(
      framework::ExecutionContext(prepared_op.op, scope, *prepared_op.dev_ctx,
                                  prepared_op.ctx, prepared_op.kernel_configs));

  std::set<std::string> vars_saved_for_backward;

  if (!stop_gradient) {
    std::unique_ptr<std::unordered_map<std::string, std::string>> grad_to_var(
        new std::unordered_map<std::string, std::string>());
    CreateGradOp(*op_desc, {}, {block}, &op->grad_op_descs_, grad_to_var.get());

    op->grad_input_vars_.resize(op->grad_op_descs_.size());
    op->grad_output_vars_.resize(op->grad_op_descs_.size());
    for (size_t i = 0; i < op->grad_op_descs_.size(); ++i) {
      framework::OpDesc* grad_op_desc = op->grad_op_descs_[i];
      for (auto it : grad_op_desc->Inputs()) {
        auto& grad_in_vars = op->grad_input_vars_[i][it.first];
        for (const std::string& grad_invar : it.second) {
          block->FindRecursiveOrCreateVar(grad_invar);
          auto var_it = grad_to_var->find(grad_invar);
          if (var_it == grad_to_var->end()) {
            auto fwd_var_it = vars.find(grad_invar);
            PADDLE_ENFORCE(fwd_var_it != vars.end());
            // Forward inputs or outputs.
            grad_in_vars.push_back(fwd_var_it->second->var_);
            vars_saved_for_backward.insert(it.first);
          } else {
            VarBase* var = vars[var_it->second];
            if (!var->grads_->var_->IsInitialized()) {
              InitVar(var->var_, var->grads_->var_,
                      prepared_op.GetDeviceContext());
            }
            // Douts.
            grad_in_vars.push_back(var->grads_->var_);
          }
        }
      }

      for (auto it : grad_op_desc->Outputs()) {
        auto& grad_out_vars = op->grad_output_vars_[i][it.first];
        for (const std::string& grad_outvar : it.second) {
          block->FindRecursiveOrCreateVar(grad_outvar);
          auto var_it = grad_to_var->find(grad_outvar);
          PADDLE_ENFORCE(var_it != grad_to_var->end(),
                         "Could not found the grad op output var, should this "
                         "operator %s's stop gradient be True",
                         op_desc->Type());
          VarBase* var = vars[var_it->second];
          if (!var->grads_->var_->IsInitialized()) {
            InitVar(var->var_, var->grads_->var_,
                    prepared_op.GetDeviceContext());
          }
          grad_out_vars.push_back(var->grads_->var_);
        }
      }
    }
  }

  op->block_ = block;
  return vars_saved_for_backward;
}

std::vector<VarBase*> Tracer::PyTrace(OpBase* op,
                                      const std::vector<VarBase*>& inputs,
                                      bool stop_gradient) {
  VLOG(3) << "py_trace";
  op->input_vars_[PyLayer::kFwdInp] = inputs;
  op->output_vars_[PyLayer::kFwdOut] = PyLayer::Apply(op->forward_id_, inputs);
  for (VarBase* inp : inputs) {
    if (inp->PreOp() && !inp->IsStopGradient()) {
      op->pre_ops_[PyLayer::kFwdInp].push_back(inp->PreOp());
      op->pre_ops_out_idx_[PyLayer::kFwdInp].push_back(inp->PreOpOutIdx());
    } else {
      op->pre_ops_[PyLayer::kFwdInp].push_back(nullptr);
    }
  }

  auto& outputs = op->output_vars_[PyLayer::kFwdOut];
  for (size_t i = 0; i < outputs.size(); ++i) {
    VarBase* out = outputs[i];
    out->TrackPreOp(op, PyLayer::kFwdOut, i, stop_gradient);
  }
  if (!stop_gradient) {
    op->grad_input_vars_.resize(1);
    op->grad_output_vars_.resize(1);
    auto& grad_input_vars =
        op->grad_input_vars_[0][framework::GradVarName(PyLayer::kFwdInp)];
    auto& grad_output_vars =
        op->grad_output_vars_[0][framework::GradVarName(PyLayer::kFwdOut)];

    for (const VarBase* inp : inputs) {
      grad_input_vars.push_back(inp->var_);
    }
    for (VarBase* out : outputs) {
      grad_input_vars.push_back(out->var_);
    }

    platform::CPUPlace place;
    for (VarBase* out : outputs) {
      grad_input_vars.push_back(out->grads_->var_);
      if (!grad_input_vars.back()->IsInitialized()) {
        // TODO(minqiyang): Add GPU support for PyLayer, only support CPU now
        InitVar(out->var_, grad_input_vars.back(),
                platform::DeviceContextPool::Instance().Get(place));
      }
    }

    for (const VarBase* inp : inputs) {
      grad_output_vars.push_back(inp->grads_->var_);
      if (!grad_output_vars.back()->IsInitialized()) {
        // TODO(minqiyang): Add GPU support for PyLayer, only support CPU now
        InitVar(inp->var_, grad_output_vars.back(),
                platform::DeviceContextPool::Instance().Get(place));
      }
    }
  }
  return outputs;
}

}  // namespace imperative
}  // namespace paddle
