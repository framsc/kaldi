// nnet3/nnet-training.cc

// Copyright      2015    Johns Hopkins University (author: Daniel Povey)
//                2015    Xiaohui Zhang

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#include "nnet3/nnet-training.h"
#include "nnet3/nnet-utils.h"
#include "nnet3/nnet-diagnostics.h"

namespace kaldi {
namespace nnet3 {

NnetTrainer::NnetTrainer(const NnetTrainerOptions &config,
                         Nnet *nnet):
    config_(config),
    nnet_(nnet),
    compiler_(*nnet, config_.optimize_config, config_.compiler_config),
    num_minibatches_processed_(0),
    srand_seed_(RandInt(0, 100000)) {
  if (config.zero_component_stats)
    ZeroComponentStats(nnet);
  KALDI_ASSERT(config.momentum >= 0.0 &&
               config.max_param_change >= 0.0);
  delta_nnet_ = nnet_->Copy();
  ScaleNnet(0.0, delta_nnet_);
  const int32 num_updatable = NumUpdatableComponents(*delta_nnet_);
  num_max_change_per_component_applied_.resize(num_updatable, 0);
  num_max_change_global_applied_ = 0;

  if (config_.read_cache != "") {
    bool binary;
    Input ki;
    if (ki.Open(config_.read_cache, &binary)) {
      compiler_.ReadCache(ki.Stream(), binary);
      KALDI_LOG << "Read computation cache from " << config_.read_cache;
    } else {
      KALDI_WARN << "Could not open cached computation. "
                    "Probably this is the first training iteration.";
    }
  }
}


void NnetTrainer::Train(const NnetExample &eg) {
  bool need_model_derivative = true;
  ComputationRequest request;
  GetComputationRequest(*nnet_, eg, need_model_derivative,
                        config_.store_component_stats,
                        &request);
  const NnetComputation *computation = compiler_.Compile(request);

  if (config_.backstitch_training_scale > 0.0 &&
      num_minibatches_processed_ % config_.backstitch_training_interval == 0) {
    // backstitch training is incompatible with momentum > 0
    KALDI_ASSERT(config_.momentum == 0.0);
    FreezeNaturalGradient(true, delta_nnet_);
    bool is_backstitch_step = true;
    srand(srand_seed_ + num_minibatches_processed_);
    ResetGenerators(nnet_);
    TrainInternal(eg, *computation, is_backstitch_step);
    FreezeNaturalGradient(false, delta_nnet_); // un-freeze natural gradient
    is_backstitch_step = false;
    srand(srand_seed_ + num_minibatches_processed_);
    ResetGenerators(nnet_);
    TrainInternal(eg, *computation, is_backstitch_step);
  } else {
    // conventional training
    bool is_backstitch_step = false;
    TrainInternal(eg, *computation, is_backstitch_step);
  }

  num_minibatches_processed_++;
}

void NnetTrainer::TrainInternal(const NnetExample &eg,
                                const NnetComputation &computation,
                                bool is_backstitch_step) {
  NnetComputer computer(config_.compute_config, computation,
                        *nnet_, delta_nnet_);
  // give the inputs to the computer object.
  computer.AcceptInputs(*nnet_, eg.io);
  computer.Run();

  this->ProcessOutputs(is_backstitch_step, eg, &computer);
  computer.Run();

  // The configurations if doing conventional training with momentum
  BaseFloat max_change_scale = 1.0, scale_adding = 1.0 - config_.momentum,
            scale_delta_nnet = config_.momentum;
  // The configurations if doing backstitch training
  if (config_.backstitch_training_scale > 0.0 && num_minibatches_processed_
      % config_.backstitch_training_interval == 0) {
    if (is_backstitch_step) {
      // max-change is scaled by backstitch_training_scale;
      // delta_nnet is scaled by -backstitch_training_scale when added to nnet;
      // delta_nnet itself is then zeroed
      max_change_scale = config_.backstitch_training_scale;
      scale_adding = -config_.backstitch_training_scale;
      scale_delta_nnet = 0.0;
    } else {
      // max-change is scaled by 1 +  backstitch_training_scale;
      // delta_nnet is scaled by 1 + backstitch_training_scale when added to nnet;
      // delta_nnet itself is then zeroed
      max_change_scale = 1.0 + config_.backstitch_training_scale;
      scale_adding = 1.0 + config_.backstitch_training_scale;
      scale_delta_nnet = 0.0;
    }
  }
  // Updates the parameters of nnet
  bool success = UpdateNnetWithMaxChange(*delta_nnet_, config_.max_param_change,
      max_change_scale, scale_adding, nnet_,
      &num_max_change_per_component_applied_, &num_max_change_global_applied_);
  // Scales deta_nnet
  if (success)
    ScaleNnet(scale_delta_nnet, delta_nnet_);
  else
    ScaleNnet(0.0, delta_nnet_);
}

void NnetTrainer::PerturbInputWithInputDeriv(const NnetExample &eg,
                                             NnetExample *eg_perturbed) {
  bool need_model_derivative = true;
  ComputationRequest request;
  GetComputationRequest(*nnet_, eg, need_model_derivative,
                        config_.store_component_stats,
                        &request);
  for (int32 i = 0; i < request.inputs.size(); i++)
    request.inputs[i].has_deriv = true;

  const NnetComputation *computation = compiler_.Compile(request);
  Nnet nnet_temp(*nnet_);
  NnetComputer computer(config_.compute_config, *computation,
                        *nnet_, &nnet_temp);
  // give the inputs to the computer object.
  computer.AcceptInputs(*nnet_, eg.io);
  computer.Run();

  this->ProcessOutputs(false, eg, &computer);
  computer.Run();

  int32 minibatch_size = GetMinibatchSize(eg);

  std::vector<BaseFloat> deriv_norm_sqr(minibatch_size, 0.0);
  for (size_t i = 0; i < eg_perturbed->io.size(); i++) {
    const NnetIo &io = eg_perturbed->io[i];
    int32 node_index = nnet_->GetNodeIndex(io.name);
    if (node_index == -1)
      KALDI_ERR << "No node named '" << io.name << "' in nnet.";
    if (nnet_->IsInputNode(node_index)) {
      const CuMatrixBase<BaseFloat> &input_deriv = computer.GetOutput(io.name);

      int32 block_size = io.features.NumRows() / minibatch_size;
      for (int32 j = 0; j < minibatch_size; j++) {
        BaseFloat norm = input_deriv.RowRange(j * block_size, block_size).FrobeniusNorm();
        deriv_norm_sqr[j] += norm * norm;
      }
    }
  }
  for (size_t i = 0; i < eg_perturbed->io.size(); i++) {
    NnetIo &io = eg_perturbed->io[i];
    int32 node_index = nnet_->GetNodeIndex(io.name);
    if (nnet_->IsInputNode(node_index)) {
      CuMatrix<BaseFloat> input_deriv;
      computer.GetOutputDestructive(io.name, &input_deriv);
      int32 block_size = io.features.NumRows() / minibatch_size;
      for (int32 j = 0; j < minibatch_size; j++) {
        if (deriv_norm_sqr[j] != 0.0) {
          BaseFloat scale = 1.0 / std::sqrt(deriv_norm_sqr[j]);
          input_deriv.RowRange(j * block_size, block_size).Scale(scale);
        }
      }
      CuMatrix<BaseFloat> cu_input(io.features.NumRows(), io.features.NumCols(),
                                   kUndefined);
      cu_input.CopyFromGeneralMat(io.features);
      cu_input.AddMat(-config_.perturb_epsilon, input_deriv);
      Matrix<BaseFloat> input(cu_input);
      io.features.SwapFullMatrix(&input);
    }
  }
}

void NnetTrainer::ProcessOutputs(bool is_backstitch_step,
                                 const NnetExample &eg,
                                 NnetComputer *computer) {
  const std::string suffix = (is_backstitch_step ? "_backstitch" : "");
  std::vector<NnetIo>::const_iterator iter = eg.io.begin(),
      end = eg.io.end();
  for (; iter != end; ++iter) {
    const NnetIo &io = *iter;
    int32 node_index = nnet_->GetNodeIndex(io.name);
    KALDI_ASSERT(node_index >= 0);
    if (nnet_->IsOutputNode(node_index)) {
      ObjectiveType obj_type = nnet_->GetNode(node_index).u.objective_type;
      BaseFloat tot_weight, tot_objf;
      bool supply_deriv = true;
      ComputeObjectiveFunction(io.features, obj_type, io.name,
                               supply_deriv, computer,
                               &tot_weight, &tot_objf);
      objf_info_[io.name + suffix].UpdateStats(io.name + suffix,
                                      config_.print_interval,
                                      num_minibatches_processed_,
                                      tot_weight, tot_objf);
      if (obj_type == kLinear) { // accuracy
        const CuMatrixBase<BaseFloat> &output = computer->GetOutput(io.name);
        BaseFloat tot_weight, tot_accuracy;
        ComputeAccuracy(io.features, output,
                        &tot_weight, &tot_accuracy);
        accuracy_info_[io.name + suffix].UpdateStats(io.name + suffix,
                                                     config_.print_interval,
                                                     num_minibatches_processed_,
                                                     tot_weight, tot_accuracy);
      }
    }
  }
}

bool NnetTrainer::PrintTotalStats() const {
  unordered_map<std::string, ObjectiveFunctionInfo, StringHasher>::const_iterator
      iter = objf_info_.begin(),
      end = objf_info_.end();
  bool ans = false;
  for (; iter != end; ++iter) {
    const std::string &name = iter->first;
    const ObjectiveFunctionInfo &info = iter->second;
    ans = ans || info.PrintTotalStats(name);
  }
  { // now print accuracies.
    ans = false;
    KALDI_LOG << "The following line is for accuracy.";
    iter = accuracy_info_.begin();
    end = accuracy_info_.end();
    for (; iter != end; ++iter) {
      const std::string &name = iter->first;
      const ObjectiveFunctionInfo &info = iter->second;
      ans = ans || info.PrintTotalStats(name);
    }
  }
  PrintMaxChangeStats();
  return ans;
}

void NnetTrainer::PrintMaxChangeStats() const {
  KALDI_ASSERT(delta_nnet_ != NULL);
  int32 i = 0;
  for (int32 c = 0; c < delta_nnet_->NumComponents(); c++) {
    Component *comp = delta_nnet_->GetComponent(c);
    if (comp->Properties() & kUpdatableComponent) {
      UpdatableComponent *uc = dynamic_cast<UpdatableComponent*>(comp);
      if (uc == NULL)
        KALDI_ERR << "Updatable component does not inherit from class "
                  << "UpdatableComponent; change this code.";
      if (num_max_change_per_component_applied_[i] > 0)
        KALDI_LOG << "For " << delta_nnet_->GetComponentName(c)
                  << ", per-component max-change was enforced "
                  << (100.0 * num_max_change_per_component_applied_[i]) /
                     num_minibatches_processed_ /
                     (config_.backstitch_training_scale > 0.0 ? 2.0 : 1.0)
                  << " \% of the time.";
      i++;
    }
  }
  if (num_max_change_global_applied_ > 0)
    KALDI_LOG << "The global max-change was enforced "
              << (100.0 * num_max_change_global_applied_) /
                 num_minibatches_processed_ /
                 (config_.backstitch_training_scale > 0.0 ? 2.0 : 1.0)
              << " \% of the time.";
}

void ObjectiveFunctionInfo::UpdateStats(
    const std::string &output_name,
    int32 minibatches_per_phase,
    int32 minibatch_counter,
    BaseFloat this_minibatch_weight,
    BaseFloat this_minibatch_tot_objf,
    BaseFloat this_minibatch_tot_aux_objf) {
  int32 phase = minibatch_counter / minibatches_per_phase;
  if (phase != current_phase) {
    KALDI_ASSERT(phase > current_phase); // or doesn't really make sense.
    PrintStatsForThisPhase(output_name, minibatches_per_phase);
    current_phase = phase;
    tot_weight_this_phase = 0.0;
    tot_objf_this_phase = 0.0;
    tot_aux_objf_this_phase = 0.0;
  }
  tot_weight_this_phase += this_minibatch_weight;
  tot_objf_this_phase += this_minibatch_tot_objf;
  tot_aux_objf_this_phase += this_minibatch_tot_aux_objf;
  tot_weight += this_minibatch_weight;
  tot_objf += this_minibatch_tot_objf;
  tot_aux_objf += this_minibatch_tot_aux_objf;
}

void ObjectiveFunctionInfo::PrintStatsForThisPhase(
    const std::string &output_name,
    int32 minibatches_per_phase) const {
  int32 start_minibatch = current_phase * minibatches_per_phase,
      end_minibatch = start_minibatch + minibatches_per_phase - 1;

  if (tot_aux_objf_this_phase == 0.0) {
    KALDI_LOG << "Average objective function for '" << output_name
              << "' for minibatches " << start_minibatch
              << '-' << end_minibatch << " is "
              << (tot_objf_this_phase / tot_weight_this_phase) << " over "
              << tot_weight_this_phase << " frames.";
  } else {
    BaseFloat objf = (tot_objf_this_phase / tot_weight_this_phase),
        aux_objf = (tot_aux_objf_this_phase / tot_weight_this_phase),
        sum_objf = objf + aux_objf;
    KALDI_LOG << "Average objective function for '" << output_name
              << "' for minibatches " << start_minibatch
              << '-' << end_minibatch << " is "
              << objf << " + " << aux_objf << " = " << sum_objf
              << " over " << tot_weight_this_phase << " frames.";
  }
}

bool ObjectiveFunctionInfo::PrintTotalStats(const std::string &name) const {
  BaseFloat objf = (tot_objf / tot_weight),
        aux_objf = (tot_aux_objf / tot_weight),
        sum_objf = objf + aux_objf;
  if (tot_aux_objf == 0.0) {
    KALDI_LOG << "Overall average objective function for '" << name << "' is "
              << (tot_objf / tot_weight) << " over " << tot_weight << " frames.";
  } else {
    KALDI_LOG << "Overall average objective function for '" << name << "' is "
              << objf << " + " << aux_objf << " = " << sum_objf
              << " over " << tot_weight << " frames.";
  }
  KALDI_LOG << "[this line is to be parsed by a script:] "
            << "log-prob-per-frame="
            << objf;
  return (tot_weight != 0.0);
}

NnetTrainer::~NnetTrainer() {
  if (config_.write_cache != "") {
    Output ko(config_.write_cache, config_.binary_write_cache);
    compiler_.WriteCache(ko.Stream(), config_.binary_write_cache);
    KALDI_LOG << "Wrote computation cache to " << config_.write_cache;
  }
  delete delta_nnet_;
}

void ComputeObjectiveFunction(const GeneralMatrix &supervision,
                              ObjectiveType objective_type,
                              const std::string &output_name,
                              bool supply_deriv,
                              NnetComputer *computer,
                              BaseFloat *tot_weight,
                              BaseFloat *tot_objf) {
  const CuMatrixBase<BaseFloat> &output = computer->GetOutput(output_name);

  if (output.NumCols() != supervision.NumCols())
    KALDI_ERR << "Nnet versus example output dimension (num-classes) "
              << "mismatch for '" << output_name << "': " << output.NumCols()
              << " (nnet) vs. " << supervision.NumCols() << " (egs)\n";

  switch (objective_type) {
    case kLinear: {
      // objective is x * y.
      switch (supervision.Type()) {
        case kSparseMatrix: {
          const SparseMatrix<BaseFloat> &post = supervision.GetSparseMatrix();
          CuSparseMatrix<BaseFloat> cu_post(post);
          // The cross-entropy objective is computed by a simple dot product,
          // because after the LogSoftmaxLayer, the output is already in the form
          // of log-likelihoods that are normalized to sum to one.
          *tot_weight = cu_post.Sum();
          *tot_objf = TraceMatSmat(output, cu_post, kTrans);
          if (supply_deriv) {
            CuMatrix<BaseFloat> output_deriv(output.NumRows(), output.NumCols(),
                                             kUndefined);
            cu_post.CopyToMat(&output_deriv);
            computer->AcceptInput(output_name, &output_deriv);
          }
          break;
        }
        case kFullMatrix: {
          // there is a redundant matrix copy in here if we're not using a GPU
          // but we don't anticipate this code branch being used in many cases.
          CuMatrix<BaseFloat> cu_post(supervision.GetFullMatrix());
          *tot_weight = cu_post.Sum();
          *tot_objf = TraceMatMat(output, cu_post, kTrans);
          if (supply_deriv)
            computer->AcceptInput(output_name, &cu_post);
          break;
        }
        case kCompressedMatrix: {
          Matrix<BaseFloat> post;
          supervision.GetMatrix(&post);
          CuMatrix<BaseFloat> cu_post;
          cu_post.Swap(&post);
          *tot_weight = cu_post.Sum();
          *tot_objf = TraceMatMat(output, cu_post, kTrans);
          if (supply_deriv)
            computer->AcceptInput(output_name, &cu_post);
          break;
        }
      }
      break;
    }
    case kQuadratic: {
      // objective is -0.5 (x - y)^2
      CuMatrix<BaseFloat> diff(supervision.NumRows(),
                               supervision.NumCols(),
                               kUndefined);
      diff.CopyFromGeneralMat(supervision);
      diff.AddMat(-1.0, output);
      *tot_weight = diff.NumRows();
      *tot_objf = -0.5 * TraceMatMat(diff, diff, kTrans);
      if (supply_deriv)
        computer->AcceptInput(output_name, &diff);
      break;
    }
    default:
      KALDI_ERR << "Objective function type " << objective_type
                << " not handled.";
  }
}



} // namespace nnet3
} // namespace kaldi
