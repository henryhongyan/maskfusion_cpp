#include "maskrcnn.h"
#include "anchors.h"
#include "debug.h"
#include "detectionlayer.h"
#include "detectiontargetlayer.h"
#include "loss.h"
#include "proposallayer.h"
#include "resnet.h"
#include "stateloader.h"

#include <cmath>
#include <experimental/filesystem>
#include <random>
#include <regex>

namespace fs = std::experimental::filesystem;

MaskRCNNImpl::MaskRCNNImpl(std::string model_dir,
                           std::shared_ptr<Config const> config)
    : model_dir_(model_dir), config_(config) {
  Build();
  InitializeWeights();
}

/* Runs the detection pipeline.
 * images: List of images, potentially of different sizes.
 * Returns a list of dicts, one dict per image. The dict contains:
 * rois: [N, (y1, x1, y2, x2)] detection bounding boxes
 * class_ids: [N] int class IDs
 * scores: [N] float probability scores for the class IDs
 * masks: [H, W, N] instance binary masks
 */
std::tuple<at::Tensor, at::Tensor> MaskRCNNImpl::Detect(
    at::Tensor images,
    const std::vector<ImageMeta>& image_metas) {
  // Run object detection
  // auto [detections, mrcnn_mask] = PredictInference(images, image_metas);
  at::Tensor detections, mrcnn_mask;
  std::tie(detections, mrcnn_mask) = PredictInference(images, image_metas);
  detections = detections.cpu();
  if (!is_empty(mrcnn_mask))
    mrcnn_mask = mrcnn_mask.permute({0, 1, 3, 4, 2}).cpu();

  return std::make_tuple(detections, mrcnn_mask);
}

std::tuple<torch::Tensor,
           torch::Tensor,
           torch::Tensor,
           torch::Tensor,
           torch::Tensor>
MaskRCNNImpl::ComputeLosses(torch::Tensor rpn_match,
                            torch::Tensor rpn_bbox,
                            torch::Tensor rpn_class_logits,
                            torch::Tensor rpn_pred_bbox,
                            torch::Tensor target_class_ids,
                            torch::Tensor mrcnn_class_logits,
                            torch::Tensor target_deltas,
                            torch::Tensor mrcnn_bbox,
                            torch::Tensor target_mask,
                            torch::Tensor mrcnn_mask) {
  auto rpn_class_loss = ComputeRpnClassLoss(rpn_match, rpn_class_logits);
  auto rpn_bbox_loss = ComputeRpnBBoxLoss(rpn_bbox, rpn_match, rpn_pred_bbox);
  auto mrcnn_class_loss =
      ComputeMrcnnClassLoss(target_class_ids, mrcnn_class_logits);
  auto mrcnn_bbox_loss =
      ComputeMrcnnBBoxLoss(target_deltas, target_class_ids, mrcnn_bbox);
  auto mrcnn_mask_loss =
      ComputeMrcnnMaskLoss(target_mask, target_class_ids, mrcnn_mask);

  return std::make_tuple(rpn_class_loss, rpn_bbox_loss, mrcnn_class_loss, mrcnn_bbox_loss,
          mrcnn_mask_loss);
}

std::tuple<std::vector<at::Tensor>, at::Tensor, at::Tensor, at::Tensor>
MaskRCNNImpl::PredictRPN(at::Tensor images, int64_t proposal_count) {
  // Feature extraction
  // auto [p2_out, p3_out, p4_out, p5_out, p6_out] = fpn_->forward(images);
  at::Tensor p2_out, p3_out, p4_out, p5_out, p6_out;
  std::tie(p2_out, p3_out, p4_out, p5_out, p6_out) = fpn_->forward(images);

  // Note that P6 is used in RPN, but not in the classifier heads.
  std::vector<at::Tensor> rpn_feature_maps = {p2_out, p3_out, p4_out, p5_out,
                                              p6_out};
  std::vector<at::Tensor> mrcnn_feature_maps = {p2_out, p3_out, p4_out, p5_out};

  // Loop through pyramid layers
  std::vector<at::Tensor> rpn_class_logits;
  std::vector<at::Tensor> rpn_class;
  std::vector<at::Tensor> rpn_bbox;
  for (auto p : rpn_feature_maps) {
    // auto [class_logits, probs, bbox] = rpn_->forward(p);
    at::Tensor class_logits, probs, bbox;
    std::tie(class_logits, probs, bbox) = rpn_->forward(p);
    rpn_class_logits.push_back(class_logits);
    rpn_class.push_back(probs);
    rpn_bbox.push_back(bbox);
  }

  // Generate proposals
  // Proposals are [batch, N, (y1, x1, y2, x2)] in normalized coordinates
  // and zero padded.
  auto scores = torch::cat(rpn_class, 1);
  auto deltas = torch::cat(rpn_bbox, 1);
  auto rpn_rois = ProposalLayer({scores, deltas}, proposal_count,
                                config_->rpn_nms_threshold, anchors_, *config_);

  auto class_logits = torch::cat(rpn_class_logits, 1);
  return std::make_tuple(mrcnn_feature_maps, rpn_rois, class_logits, deltas);
}

std::tuple<at::Tensor,
           at::Tensor,
           at::Tensor,
           at::Tensor,
           at::Tensor,
           at::Tensor,
           at::Tensor,
           at::Tensor>
MaskRCNNImpl::PredictTraining(at::Tensor images,
                              at::Tensor gt_class_ids,
                              at::Tensor gt_boxes,
                              at::Tensor gt_masks) {
  train();
  // Set batchnorm always in eval mode during training
  auto set_bn_eval = [](const std::string& /*name*/, Module& m) {
    if (m.name().find("BatchNorm") != std::string::npos) {
      m.eval();
    }
  };
  apply(set_bn_eval);

  /*
  auto [mrcnn_feature_maps, rpn_rois, rpn_class_logits, rpn_bbox] =
      PredictRPN(images, config_->post_nms_rois_training);
  */
  std::vector<at::Tensor> mrcnn_feature_maps;
  at::Tensor rpn_rois, rpn_class_logits, rpn_bbox;
  std::tie(mrcnn_feature_maps, rpn_rois, rpn_class_logits, rpn_bbox) =
          PredictRPN(images, config_->post_nms_rois_training);

  // Debug block
  //  auto d = torch::tensor({512, 512, 512, 512}, at::dtype(at::kFloat));
  //  VisualizeBoxes("rpn_targets", 512, 512,
  //                 rpn_rois.squeeze().cpu().narrow(0, 0, 10) * d,
  //                 gt_boxes.squeeze().cpu());
  // exit(0);

  // Normalize coordinates
  auto h = static_cast<float>(config_->image_shape[0]);
  auto w = static_cast<float>(config_->image_shape[1]);
  auto scale =
      torch::tensor({h, w, h, w}, at::dtype(at::kFloat).requires_grad(false));
  if (config_->gpu_count > 0)
    scale = scale.cuda();
  gt_boxes = gt_boxes / scale;

  // Generate detection targets
  // Subsamples proposals and generates target outputs for training
  // Note that proposal class IDs, gt_boxes, and gt_masks are zero
  // padded. Equally, returned rois and targets are zero padded.
  /*
  auto [rois, target_class_ids, target_deltas, target_mask] =
      DetectionTargetLayer(*config_, rpn_rois, gt_class_ids, gt_boxes,
                           gt_masks);
  */
  at::Tensor rois, target_class_ids, target_deltas, target_mask;
  std::tie(rois, target_class_ids, target_deltas, target_mask) =
      DetectionTargetLayer(*config_, rpn_rois, gt_class_ids, gt_boxes,
                           gt_masks);

  // Debug block
  //  VisualizeBoxes("proposals", 512, 512, rois.squeeze().cpu() * d,
  //                 gt_boxes.squeeze().cpu() * d);
  //  exit(0);

  auto mrcnn_class_logits = torch::empty({}, at::dtype(at::kFloat));
  auto mrcnn_class = torch::empty({}, at::dtype(at::kInt));
  auto mrcnn_bbox = torch::empty({}, at::dtype(at::kFloat));
  auto mrcnn_mask = torch::empty({}, at::dtype(at::kFloat));
  if (config_->gpu_count > 0) {
    mrcnn_class_logits = mrcnn_class_logits.cuda();
    mrcnn_class = mrcnn_class.cuda();
    mrcnn_bbox = mrcnn_bbox.cuda();
    mrcnn_mask = mrcnn_mask.cuda();
  }

  if (!is_empty(rois)) {
    // Network Heads
    // Proposal classifier and BBox regressor heads
    std::tie(mrcnn_class_logits, mrcnn_class, mrcnn_bbox) =
        classifier_->forward(mrcnn_feature_maps, rois);

    // Add back batch dimension
    rois = rois.unsqueeze(0);

    // Create masks for detections
    mrcnn_mask = mask_->forward(mrcnn_feature_maps, rois);
  }

  return std::make_tuple(rpn_class_logits, rpn_bbox,   target_class_ids, mrcnn_class_logits,
          target_deltas,    mrcnn_bbox, target_mask,      mrcnn_mask);
}

std::tuple<at::Tensor, at::Tensor> MaskRCNNImpl::PredictInference(
    at::Tensor images,
    const std::vector<ImageMeta>& image_metas) {
  eval();

  /*
  auto [mrcnn_feature_maps, rpn_rois, rpn_class_logits, rpn_bbox] =
      PredictRPN(images, config_->post_nms_rois_inference);
  */
  std::vector<at::Tensor> mrcnn_feature_maps;
  at::Tensor rpn_rois, rpn_class_logits, rpn_bbox;
  std::tie(mrcnn_feature_maps, rpn_rois, rpn_class_logits, rpn_bbox) =
      PredictRPN(images, config_->post_nms_rois_inference);
      
  // Network Heads
  // Proposal classifier and BBox regressor heads
  /*
  auto [mrcnn_class_logits, mrcnn_class, mrcnn_bbox] =
      classifier_->forward(mrcnn_feature_maps, rpn_rois);
  */
  at::Tensor mrcnn_class_logits, mrcnn_class, mrcnn_bbox;
  std::tie(mrcnn_class_logits, mrcnn_class, mrcnn_bbox) =
      classifier_->forward(mrcnn_feature_maps, rpn_rois);

  // Detections
  // output is [batch, num_detections, (y1, x1, y2, x2, class_id, score)] in
  // image coordinates
  at::Tensor detections = DetectionLayer(*config_.get(), rpn_rois, mrcnn_class,
                                         mrcnn_bbox, image_metas);

  auto mrcnn_mask = torch::empty({0}, at::dtype(at::kFloat));
  if (!is_empty(detections)) {
    // Convert boxes to normalized coordinates
    auto h = static_cast<float>(config_->image_shape[0]);
    auto w = static_cast<float>(config_->image_shape[1]);

    auto scale =
        torch::tensor({h, w, h, w}, at::dtype(at::kFloat).requires_grad(false));

    if (config_->gpu_count > 0)
      scale = scale.cuda();
    auto detection_boxes = detections.narrow(1, 0, 4) / scale;

    // Add back batch dimension
    detection_boxes = detection_boxes.unsqueeze(0);

    // Create masks for detections
    mrcnn_mask = mask_->forward(mrcnn_feature_maps, detection_boxes);

    // Add back batch dimension
    detections = detections.unsqueeze(0);
    mrcnn_mask = mrcnn_mask.unsqueeze(0);
  }
  return std::make_tuple(detections, mrcnn_mask);
}

// Build Mask R-CNN architecture.
void MaskRCNNImpl::Build() {
  assert(config_);

  // Image size must be dividable by 2 multiple times
  auto h = config_->image_shape[0];
  auto w = config_->image_shape[1];
  auto p = static_cast<int32_t>(std::pow(2l, 6l));
  if (static_cast<int32_t>(static_cast<double>(h) / static_cast<double>(p)) !=
          h / p ||
      static_cast<int32_t>(static_cast<double>(w) / static_cast<double>(p)) !=
          w / p) {
    throw std::invalid_argument(
        "Image size must be dividable by 2 at least 6 times "
        "to avoid fractions when downscaling and upscaling."
        "For example, use 256, 320, 384, 448, 512, ... etc. ");
  }

  // Build the shared convolutional layers.
  // Bottom-up Layers
  // Returns a list of the last layers of each stage, 5 in total.
  // Don't create the thead (stage 5), so we pick the 4th item in the list.
  ResNetImpl resnet(ResNetImpl::Architecture::ResNet101, true);
  // auto [C1, C2, C3, C4, C5] = resnet.GetStages();
  torch::nn::Sequential C1{nullptr};
  torch::nn::Sequential C2{nullptr};
  torch::nn::Sequential C3{nullptr};
  torch::nn::Sequential C4{nullptr};
  torch::nn::Sequential C5{nullptr};
  std::tie(C1, C2, C3, C4, C5) = resnet.GetStages();

  // Top-down Layers
  // TODO: (Legacy)add assert to varify feature map sizes match what's in config
  fpn_ = FPN(C1, C2, C3, C4, C5, /*out_channels*/ 256);
  register_module("fpn", fpn_);

  anchors_ = GeneratePyramidAnchors(
      config_->rpn_anchor_scales, config_->rpn_anchor_ratios,
      config_->backbone_shapes, config_->backbone_strides,
      config_->rpn_anchor_stride);

  if (config_->gpu_count > 0)
    anchors_ = anchors_.toBackend(torch::Backend::CUDA);

  // RPN
  rpn_ =
      RPN(config_->rpn_anchor_ratios.size(), config_->rpn_anchor_stride, 256);
  register_module("rpn", rpn_);

  // FPN Classifier
  classifier_ = Classifier(256, config_->pool_size, config_->image_shape,
                           config_->num_classes);
  register_module("classifier", classifier_);

  // FPN Mask
  mask_ = Mask(256, config_->mask_pool_size, config_->image_shape,
               config_->num_classes);
  register_module("mask", mask_);

  // Fix batch norm layers
  auto set_bn_fix = [](const std::string& name, Module& m) {
    if (name.find("BatchNorm") != std::string::npos) {
      for (auto& p : m.parameters())
        p.set_requires_grad(false);
    }
  };

  apply(set_bn_fix);
}

void MaskRCNNImpl::InitializeWeights() {
  for (auto m : modules(false)) {
    if (m->name().find("Conv2d") != std::string::npos) {
      for (auto& p : m->named_parameters()) {
        if (p.key().find("weight") != std::string::npos) {
          torch::nn::init::xavier_uniform_(p.value());
        } else if (p.key().find("bias") != std::string::npos) {
          torch::nn::init::zeros_(p.value());
        }
      }
    } else if (m->name().find("BatchNorm2d") != std::string::npos) {
      for (auto& p : m->named_parameters()) {
        if (p.key().find("weight") != std::string::npos) {
          torch::nn::init::ones_(p.value());
        }
        if (p.key().find("bias") != std::string::npos) {
          torch::nn::init::zeros_(p.value());
        }
      }
    } else if (m->name().find("Linear") != std::string::npos) {
      for (auto& p : m->named_parameters()) {
        if (p.key().find("weight") != std::string::npos) {
          torch::nn::init::normal_(p.value(), 0, 0.01);
        }
        if (p.key().find("bias") != std::string::npos) {
          torch::nn::init::zeros_(p.value());
        }
      }
    }
  }
}

void MaskRCNNImpl::SetTrainableLayers(const std::string& layers_regex) {
  std::regex re(layers_regex);
  std::smatch m;
  auto params = named_parameters(true /*recurse*/);
  for (auto& param : params) {
    auto layer_name = param.key();
    bool is_trainable = std::regex_match(layer_name, m, re);
    if (!is_trainable) {
      param.value().set_requires_grad(false);
    }
  }
}

std::string MaskRCNNImpl::GetCheckpointPath(uint32_t epoch) const {
  return fs::path(model_dir_) /
         ("checkpoint_epoch_" + std::to_string(epoch) + ".pt");
}
