#include "frcnn_api.hpp"
using caffe::Blob;

void Detector::preprocess(const cv::Mat &img_in, const int blob_idx) {
    const vector<Blob<float> *> &input_blobs = net_->input_blobs();
    CHECK(img_in.isContinuous()) << "Warning : cv::Mat img_out is not Continuous !";
    DLOG(ERROR) << "img_in (CHW) : " << img_in.channels() << ", " << img_in.rows << ", " << img_in.cols;
    input_blobs[blob_idx]->Reshape(1, img_in.channels(), img_in.rows, img_in.cols);
    float *blob_data = input_blobs[blob_idx]->mutable_cpu_data();
    const int cols = img_in.cols;
    const int rows = img_in.rows;
    for (int i = 0; i < cols * rows; i++) {
        blob_data[cols * rows * 0 + i] =
                reinterpret_cast<float *>(img_in.data)[i * 3 + 0];// mean_[0];
        blob_data[cols * rows * 1 + i] =
                reinterpret_cast<float *>(img_in.data)[i * 3 + 1];// mean_[1];
        blob_data[cols * rows * 2 + i] =
                reinterpret_cast<float *>(img_in.data)[i * 3 + 2];// mean_[2];
    }
}

void Detector::preprocess(const vector<float> &data, const int blob_idx) {
    const vector<Blob<float> *> &input_blobs = net_->input_blobs();
    input_blobs[blob_idx]->Reshape(1, data.size(), 1, 1);
    float *blob_data = input_blobs[blob_idx]->mutable_cpu_data();
    std::memcpy(blob_data, &data[0], sizeof(float) * data.size());
}

void Detector::Set_Model(std::string &proto_file, std::string &model_file, std::string default_config) {
    FrcnnParam::load_param(default_config);
    net_.reset(new Net<float>(proto_file, caffe::TEST));
    net_->CopyTrainedLayersFrom(model_file);
    mean_[0] = FrcnnParam::pixel_means[0];
    mean_[1] = FrcnnParam::pixel_means[1];
    mean_[2] = FrcnnParam::pixel_means[2];
    DLOG(INFO) << "SET MODEL DONE";
    FrcnnParam::print_param();
}

vector<boost::shared_ptr<Blob<float> > > Detector::predict(const vector<std::string> blob_names) {
    DLOG(ERROR) << "FORWARD BEGIN";
    float loss;
    net_->Forward(&loss);
    vector<boost::shared_ptr<Blob<float> > > output;
    for (int i = 0; i < blob_names.size(); ++i) {
        output.push_back(this->net_->blob_by_name(blob_names[i]));
    }
    DLOG(ERROR) << "FORWARD END, Loss : " << loss;
    return output;
}

void Detector::predict(const cv::Mat &img_in, vector<BBox<float>> &results) {

    float scale_factor ;
    cv::Mat img;
    const int height = img_in.rows;
    const int width = img_in.cols;
    DLOG(INFO) << "height: " << height << " width: " << width;
    img_in.convertTo(img, CV_32FC3);
    for (int r = 0; r < img.rows; r++) {
        for (int c = 0; c < img.cols; c++) {
            int offset = (r * img.cols + c) * 3;
            reinterpret_cast<float *>(img.data)[offset + 0] -= this->mean_[0]; // B
            reinterpret_cast<float *>(img.data)[offset + 1] -= this->mean_[1]; // G
            reinterpret_cast<float *>(img.data)[offset + 2] -= this->mean_[2]; // R
        }
    }
    {
        int scale_multiple_of = FrcnnParam::scale_multiple_of;
        vector<float> im_scales(2) ;
        get_scale_factor(img_in.cols, img_in.rows, FrcnnParam::test_scales[0],FrcnnParam::test_max_size,
                                       scale_multiple_of,im_scales);
        //std::cout <<"im_scale_x " <<im_scales[0] << im_scales[1] << std::endl;
        scale_factor =  (im_scales[0] < im_scales[1])?im_scales[0]:im_scales[1] ;
        cv::resize(img, img, cv::Size(), im_scales[0], im_scales[1]);
    }
    //cv::resize(img, img, cv::Size(), scale_factor, scale_factor);

    std::vector<float> im_info(3);
    im_info[0] = img.rows;
    im_info[1] = img.cols;
    im_info[2] = scale_factor;

    DLOG(ERROR) << "im_info : " << im_info[0] << ", " << im_info[1] << ", " << im_info[2];
    this->preprocess(img, 0);
    this->preprocess(im_info, 1);

    vector<std::string> blob_names(3);
    blob_names[0] = "rois";
    blob_names[1] = "cls_prob";
    blob_names[2] = "bbox_pred";

    vector<boost::shared_ptr<Blob<float> > > output = this->predict(blob_names);
    boost::shared_ptr<Blob<float> > rois(output[0]);
    boost::shared_ptr<Blob<float> > cls_prob(output[1]);
    boost::shared_ptr<Blob<float> > bbox_pred(output[2]);

    const int box_num = bbox_pred->num();
    const int cls_num = cls_prob->channels();
    CHECK_EQ(cls_num, FrcnnParam::n_classes);
    results.clear();

    for (int cls = 1; cls < cls_num; cls++) {
        vector<BBox<float> > bbox;
        for (int i = 0; i < box_num; i++) {
            float score = cls_prob->cpu_data()[i * cls_num + cls];

            Point4f<float> roi(rois->cpu_data()[(i * 5) + 1] / scale_factor,
                               rois->cpu_data()[(i * 5) + 2] / scale_factor,
                               rois->cpu_data()[(i * 5) + 3] / scale_factor,
                               rois->cpu_data()[(i * 5) + 4] / scale_factor);

            Point4f<float> delta(bbox_pred->cpu_data()[(i * cls_num + cls) * 4 + 0],
                                 bbox_pred->cpu_data()[(i * cls_num + cls) * 4 + 1],
                                 bbox_pred->cpu_data()[(i * cls_num + cls) * 4 + 2],
                                 bbox_pred->cpu_data()[(i * cls_num + cls) * 4 + 3]);

            Point4f<float> box = bbox_transform_inv(roi, delta);
            box[0] = std::max(0.0f, box[0]);
            box[1] = std::max(0.0f, box[1]);
            box[2] = std::min(width - 1.f, box[2]);
            box[3] = std::min(height - 1.f, box[3]);

            // BBox tmp(box, score, cls);
            // LOG(ERROR) << "cls: " << tmp.id << " score: " << tmp.confidence;
            // LOG(ERROR) << "roi: " << roi.to_string();
            bbox.push_back(BBox<float>(box, score, cls));
        }
        sort(bbox.begin(), bbox.end());
        vector<bool> select(box_num, true);
        // Apply NMS
        for (int i = 0; i < box_num; i++)
            if (select[i]) {
                if (bbox[i].Confidence() < FrcnnParam::test_score_thresh) break;
                for (int j = i + 1; j < box_num; j++) {
                    if (select[j]) {
                        if (get_iou(bbox[i], bbox[j]) > FrcnnParam::test_nms) {
                            select[j] = false;
                        }
                    }
                }
                results.push_back(bbox[i]);
            }
    }

}


