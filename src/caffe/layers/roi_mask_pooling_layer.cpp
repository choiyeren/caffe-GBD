// ------------------------------------------------------------------
// Fast R-CNN
// Copyright (c) 2015 Microsoft
// Licensed under The MIT License [see fast-rcnn/LICENSE for details]
// Written by Ross Girshick
// ------------------------------------------------------------------

#include <cfloat>

#include "caffe/vision_layers.hpp"

using std::max;
using std::min;
using std::floor;
using std::ceil;

namespace caffe {

template <typename Dtype>
void ROIMaskPoolingLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  ROIMaskPoolingParameter roi_mask_pool_param =
      this->layer_param_.roi_mask_pooling_param();
  CHECK_GT(roi_mask_pool_param.pooled_h(), 0)
      << "pooled_h must be > 0";
  CHECK_GT(roi_mask_pool_param.pooled_w(), 0)
      << "pooled_w must be > 0";
  pooled_height_ = roi_mask_pool_param.pooled_h();
  pooled_width_ = roi_mask_pool_param.pooled_w();
  spatial_scale_ = roi_mask_pool_param.spatial_scale();
  spatial_shift_ = roi_mask_pool_param.spatial_shift();
  half_part_ = roi_mask_pool_param.half_part();
  roi_scale_ = roi_mask_pool_param.roi_scale();
  mask_scale_ = roi_mask_pool_param.mask_scale();
  LOG(INFO) << "Spatial scale: " << spatial_scale_;
  LOG(INFO) << "Spatial shift: " << spatial_shift_;
}

template <typename Dtype>
void ROIMaskPoolingLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  channels_ = bottom[0]->channels();
  height_ = bottom[0]->height();
  width_ = bottom[0]->width();
  top[0]->Reshape(bottom[1]->num(), channels_, pooled_height_,
      pooled_width_);
  max_idx_.Reshape(bottom[1]->num(), channels_, pooled_height_,
      pooled_width_);
}

template <typename Dtype>
void ROIMaskPoolingLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const Dtype* bottom_data = bottom[0]->cpu_data();
  const Dtype* bottom_rois = bottom[1]->cpu_data();
  // Number of ROIs
  int num_rois = bottom[1]->num();
  int batch_size = bottom[0]->num();
  int top_count = top[0]->count();
  Dtype* top_data = top[0]->mutable_cpu_data();
  caffe_set(top_count, Dtype(-FLT_MAX), top_data);
  int* argmax_data = max_idx_.mutable_cpu_data();
  caffe_set(top_count, -1, argmax_data);

  // For each ROI R = [batch_index x1 y1 x2 y2]: max pool over R
  for (int n = 0; n < num_rois; ++n) {
    int roi_batch_ind = bottom_rois[0];
    CHECK_GE(roi_batch_ind, 0);
    CHECK_LT(roi_batch_ind, batch_size);
    Dtype x1 = bottom_rois[1];
    Dtype x2 = bottom_rois[2];
    Dtype y1 = bottom_rois[3];
    Dtype y2 = bottom_rois[4];
    Dtype xc = (x1 + x2) / 2;
    Dtype yc = (y1 + y2) / 2;
    Dtype w = x2 - x1;
    Dtype h = y2 - y1;
    // rescale roi with regard to roi_scale and half_part
    Dtype xx1 = xc - w * roi_scale_ / 2;
    Dtype xx2 = xc + w * roi_scale_ / 2;
    Dtype yy1 = yc - h * roi_scale_ / 2;
    Dtype yy2 = yc + h * roi_scale_ / 2;
    switch (half_part_) {
        case 0: break;
        case 1: xx2 = xc; break;
        case 2: xx1 = xc; break;
        case 3: yy2 = yc; break;
        case 4: yy1 = yc; break;
        default: break;
    }
    // rescaled roi/mask size on conv feature map
    int roi_start_w = round(xx1 * spatial_scale_ + spatial_shift_);
    int roi_start_h = round(yy1 * spatial_scale_ + spatial_shift_);
    int roi_end_w = round(xx2 * spatial_scale_ + spatial_shift_);
    int roi_end_h = round(yy2 * spatial_scale_ + spatial_shift_);

    // rescale mask with regard to mask_scale
    bool isMask = mask_scale_ > 0.0;
    Dtype mx1 = xc - w * mask_scale_ / 2.0;
    Dtype mx2 = xc + w * mask_scale_ / 2.0;
    Dtype my1 = yc - h * mask_scale_ / 2.0;
    Dtype my2 = yc + h * mask_scale_ / 2.0;

    int mask_start_w = round(mx1 * spatial_scale_ + spatial_shift_);
    int mask_start_h = round(my1 * spatial_scale_ + spatial_shift_);
    int mask_end_w = round(mx2 * spatial_scale_ + spatial_shift_);
    int mask_end_h = round(my2 * spatial_scale_ + spatial_shift_);

    int roi_height = max(roi_end_h - roi_start_h + 1, 1);
    int roi_width = max(roi_end_w - roi_start_w + 1, 1);
    const Dtype bin_size_h = static_cast<Dtype>(roi_height)
                             / static_cast<Dtype>(pooled_height_);
    const Dtype bin_size_w = static_cast<Dtype>(roi_width)
                             / static_cast<Dtype>(pooled_width_);

    const Dtype* batch_data = bottom_data + bottom[0]->offset(roi_batch_ind);

    for (int c = 0; c < channels_; ++c) {
      for (int ph = 0; ph < pooled_height_; ++ph) {
        for (int pw = 0; pw < pooled_width_; ++pw) {
          // Compute pooling region for this output unit:
          //  start (included) = floor(ph * roi_height / pooled_height_)
          //  end (excluded) = ceil((ph + 1) * roi_height / pooled_height_)
          int hstart = static_cast<int>(floor(static_cast<Dtype>(ph)
                                              * bin_size_h));
          int wstart = static_cast<int>(floor(static_cast<Dtype>(pw)
                                              * bin_size_w));
          int hend = static_cast<int>(ceil(static_cast<Dtype>(ph + 1)
                                           * bin_size_h));
          int wend = static_cast<int>(ceil(static_cast<Dtype>(pw + 1)
                                           * bin_size_w));

          hstart = min(max(hstart + roi_start_h, 0), height_);
          hend = min(max(hend + roi_start_h, 0), height_);
          wstart = min(max(wstart + roi_start_w, 0), width_);
          wend = min(max(wend + roi_start_w, 0), width_);

          bool is_empty = (hend <= hstart) || (wend <= wstart);

          const int pool_index = ph * pooled_width_ + pw;
          if (is_empty) {
            top_data[pool_index] = 0;
            argmax_data[pool_index] = -1;
          }

          for (int h = hstart; h < hend; ++h) {
            for (int w = wstart; w < wend; ++w) {
              const int index = h * width_ + w;
              Dtype value = batch_data[index];
              // apply mask
              if (isMask) {
                if (w >= mask_start_w && w <= mask_end_w
                    && h >= mask_start_h && h <= mask_end_h) {
                  value = 0;
                }
              }
              if (value > top_data[pool_index]) {
                top_data[pool_index] = value;
                argmax_data[pool_index] = index;
              }
            }
          }
        }
      }
      // Increment all data pointers by one channel
      batch_data += bottom[0]->offset(0, 1);
      top_data += top[0]->offset(0, 1);
      argmax_data += max_idx_.offset(0, 1);
    }
    // Increment ROI data pointer
    bottom_rois += bottom[1]->offset(1);
  }
}

template <typename Dtype>
void ROIMaskPoolingLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
      const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
  NOT_IMPLEMENTED;
}


#ifdef CPU_ONLY
STUB_GPU(ROIMaskPoolingLayer);
#endif

INSTANTIATE_CLASS(ROIMaskPoolingLayer);
REGISTER_LAYER_CLASS(ROIMaskPooling);

}  // namespace caffe
