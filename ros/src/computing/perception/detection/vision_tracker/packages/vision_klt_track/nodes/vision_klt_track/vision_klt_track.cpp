/*
 * Copyright 2015-2019 Autoware Foundation. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// ROS STUFF
#include <ros/ros.h>

#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>

#include <autoware_msgs/ImageObj.h>
#include <autoware_msgs/ImageObjRanged.h>
#include <autoware_msgs/ImageObjTracked.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>

// TRACKING STUFF
#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/objdetect/objdetect.hpp>
#include <opencv2/video/tracking.hpp>

#include <LkTracker.hpp>

#include <iostream>
#include <stdio.h>

#include <algorithm>
#include <iterator>
#include <sstream>
#include <string>

#include "gencolors.cpp"

class ROSTrackerApp {
  ros::Subscriber subscriber_image_raw_;
  ros::Subscriber subscriber_image_obj_;
  ros::Subscriber subscriber_klt_config_;
  ros::Publisher publisher_tracked_objects_; // ROS
  ros::NodeHandle node_handle_;

  std::string tracked_type_;

  bool ready_;

  bool track_ready_;
  bool detect_ready_;

  int num_trackers_;

  std::vector<LkTracker *> obj_trackers_;
  std::vector<ObjectDetection> obj_detections_;

  std::vector<float> ranges_;
  std::vector<float> min_heights_;
  std::vector<float> max_heights_;

  autoware_msgs::ImageObjTracked ros_objects_msg_; // sync

  void Sort(const std::vector<float> in_scores,
            std::vector<unsigned int> &in_out_indices) {
    for (unsigned int i = 0; i < in_scores.size(); i++)
      for (unsigned int j = i + 1; j < in_scores.size(); j++) {
        if (in_scores[in_out_indices[j]] > in_scores[in_out_indices[i]]) {
          // float x_tmp = x[i];
          int index_tmp = in_out_indices[i];
          // x[i] = x[j];
          in_out_indices[i] = in_out_indices[j];
          // x[j] = x_tmp;
          in_out_indices[j] = index_tmp;
        }
      }
  }

  void ApplyNonMaximumSuppresion(std::vector<LkTracker *> &in_out_source,
                                 float in_nms_threshold) {
    if (in_out_source.empty())
      return;

    unsigned int size = in_out_source.size();

    std::vector<float> area(size);
    std::vector<float> scores(size);
    std::vector<int> x1(size);
    std::vector<int> y1(size);
    std::vector<int> x2(size);
    std::vector<int> y2(size);
    std::vector<unsigned int> indices(size);
    std::vector<bool> is_suppresed(size);

    for (unsigned int i = 0; i < in_out_source.size(); i++) {
      ObjectDetection tmp = in_out_source[i]->GetTrackedObject();
      area[i] = tmp.rect.width * tmp.rect.height;
      if (area[i] > 0)
        is_suppresed[i] = false;
      else {
        is_suppresed[i] = true;
        in_out_source[i]->NullifyLifespan();
      }
      indices[i] = i;
      scores[i] = tmp.score;
      x1[i] = tmp.rect.x;
      y1[i] = tmp.rect.y;
      x2[i] = tmp.rect.width + tmp.rect.x;
      y2[i] = tmp.rect.height + tmp.rect.y;
    }

    Sort(area, indices); // returns indices ordered based on scores

    for (unsigned int i = 0; i < size; i++) {

      for (unsigned int j = i + 1; j < size; j++) {
        if (is_suppresed[indices[i]] || is_suppresed[indices[j]])
          continue;
        int x1_max = std::max(x1[indices[i]], x1[indices[j]]);
        int x2_min = std::min(x2[indices[i]], x2[indices[j]]);
        int y1_max = std::max(y1[indices[i]], y1[indices[j]]);
        int y2_min = std::min(y2[indices[i]], y2[indices[j]]);
        int overlap_width = x2_min - x1_max + 1;
        int overlap_height = y2_min - y1_max + 1;
        if (overlap_width > 0 && overlap_height > 0) {
          float overlap_part =
              (overlap_width * overlap_height) / area[indices[j]];
          if (overlap_part > in_nms_threshold) {
            is_suppresed[indices[j]] = true;
            in_out_source[indices[j]]->NullifyLifespan();
            if (in_out_source[indices[j]]->GetFrameCount() >
                in_out_source[indices[i]]->GetFrameCount()) {
              in_out_source[indices[i]]->object_id =
                  in_out_source[indices[j]]->object_id;
            }
          }
        }
      }
    }
    return;
  }

  void publish_if_possible() {
    if (track_ready_ && detect_ready_) {
      publisher_tracked_objects_.publish(ros_objects_msg_);
      track_ready_ = false;
      detect_ready_ = false;
    }
  }

public:
  void image_callback(const sensor_msgs::Image &image_source) {
    cv_bridge::CvImagePtr cv_image =
        cv_bridge::toCvCopy(image_source, sensor_msgs::image_encodings::BGR8);
    cv::Mat image_track = cv_image->image;

    ObjectDetection empty_detection;
    empty_detection.rect = cv::Rect(0, 0, 0, 0);
    empty_detection.score = 0;
    unsigned int i;

    std::vector<bool> tracker_matched(obj_trackers_.size(), false);
    std::vector<bool> object_matched(obj_detections_.size(), false);

    // check object detections vs current trackers
    for (i = 0; i < obj_detections_.size(); i++) {
      for (unsigned int j = 0; j < obj_trackers_.size(); j++) {
        if (tracker_matched[j] || object_matched[i])
          continue;

        ObjectDetection tmp_detection = obj_detections_[i];
        int area = tmp_detection.rect.width * tmp_detection.rect.height;
        cv::Rect intersection =
            tmp_detection.rect & obj_trackers_[j]->GetTrackedObject().rect;
        if ((intersection.width * intersection.height) > area * 0.3) {

          obj_trackers_[j]->Track(image_track, obj_detections_[i], true);
          tracker_matched[j] = true;
          object_matched[i] = true;
          // std::cout << "matched " << i << " with " << j << std::endl;
        }
      }
    }

    // run the trackers not matched
    for (i = 0; i < obj_trackers_.size(); i++) {
      if (!tracker_matched[i]) {
        obj_trackers_[i]->Track(image_track, empty_detection, false);
      }
    }

    // create trackers for those objects not being tracked yet
    for (unsigned int i = 0; i < obj_detections_.size(); i++) {
      if (!object_matched[i]) // if object wasn't matched by overlapping area,
                              // create a new tracker
      {
        if (num_trackers_ > 10)
          num_trackers_ = 0;
        LkTracker *new_tracker = new LkTracker(++num_trackers_, min_heights_[i],
                                               max_heights_[i], ranges_[i]);
        new_tracker->Track(image_track, obj_detections_[i], true);

        // std::cout << "added new tracker" << std::endl;
        obj_trackers_.push_back(new_tracker);
      }
    }

    ApplyNonMaximumSuppresion(obj_trackers_, 0.3);

    // remove those trackers with its lifespan <=0
    std::vector<LkTracker *>::iterator it;
    for (it = obj_trackers_.begin(); it != obj_trackers_.end();) {
      if ((*it)->GetRemainingLifespan() <= 0) {
        it = obj_trackers_.erase(it);
        // std::cout << "deleted a tracker " << std::endl;
      } else
        it++;
    }

    // copy results to ros msg
    unsigned int num = obj_trackers_.size();
    std::vector<autoware_msgs::ImageRectRanged>
        rect_ranged_array;              // tracked rectangles
    std::vector<int> real_data(num, 0); // boolean array to show if data in
                                        // rect_ranged comes from tracking or
                                        // detection
    std::vector<unsigned int> obj_id(num, 0); // id number for each rect_range
    std::vector<unsigned int> lifespan(
        num, 0); // remaining lifespan of each rectranged
    for (i = 0; i < num; i++) {
      autoware_msgs::ImageRectRanged rect_ranged;
      LkTracker tracker_tmp = *obj_trackers_[i];
      rect_ranged.rect.x = tracker_tmp.GetTrackedObject().rect.x;
      rect_ranged.rect.y = tracker_tmp.GetTrackedObject().rect.y;
      rect_ranged.rect.width = tracker_tmp.GetTrackedObject().rect.width;
      rect_ranged.rect.height = tracker_tmp.GetTrackedObject().rect.height;
      rect_ranged.rect.score = tracker_tmp.GetTrackedObject().score;
      rect_ranged.max_height = tracker_tmp.max_height_;
      rect_ranged.min_height = tracker_tmp.min_height_;
      rect_ranged.range = tracker_tmp.range_;

      rect_ranged_array.push_back(rect_ranged);

      lifespan[i] = tracker_tmp.GetRemainingLifespan();
      obj_id[i] = tracker_tmp.object_id;
      if (lifespan[i] == tracker_tmp.DEFAULT_LIFESPAN_)
        real_data[i] = 1;

      cv::rectangle(image_track, tracker_tmp.GetTrackedObject().rect,
                    cv::Scalar(0, 255, 0), 2);
    }

    // std::cout << "TRACKERS: " << obj_trackers_.size() << std::endl;

    obj_detections_.clear();
    ranges_.clear();

    autoware_msgs::ImageObjTracked tmp_objects_msg;

    tmp_objects_msg.type = tracked_type_;
    tmp_objects_msg.total_num = num;
    copy(rect_ranged_array.begin(), rect_ranged_array.end(),
         back_inserter(tmp_objects_msg.rect_ranged)); // copy vector
    copy(real_data.begin(), real_data.end(),
         back_inserter(tmp_objects_msg.real_data)); // copy vector
    copy(obj_id.begin(), obj_id.end(),
         back_inserter(tmp_objects_msg.obj_id)); // copy vector
    copy(lifespan.begin(), lifespan.end(),
         back_inserter(tmp_objects_msg.lifespan)); // copy vector

    tmp_objects_msg.header = image_source.header;

    ros_objects_msg_ = tmp_objects_msg;

    // publisher_tracked_objects_.publish(ros_objects_msg);

    // cv::imshow("KLT",image_track);
    // cv::waitKey(1);

    track_ready_ = true;
    // ready_ = false;

    publish_if_possible();
  }

  void detections_callback(autoware_msgs::ImageObjRanged image_objects_msg) {
    // if(ready_)
    //	return;
    if (!detect_ready_) // must NOT overwrite, data is probably being used by
                        // tracking.
    {
      unsigned int num = image_objects_msg.obj.size();
      std::vector<autoware_msgs::ImageRectRanged> objects =
          image_objects_msg.obj;
      tracked_type_ = image_objects_msg.type;
      // points are X,Y,W,H and repeat for each instance
      obj_detections_.clear();
      ranges_.clear();

      for (unsigned int i = 0; i < num; i++) {
        cv::Rect tmp;
        tmp.x = objects.at(i).rect.x;
        tmp.y = objects.at(i).rect.y;
        tmp.width = objects.at(i).rect.width;
        tmp.height = objects.at(i).rect.height;
        ObjectDetection tmp_obj;
        tmp_obj.rect = tmp;
        tmp_obj.score = 0;
        obj_detections_.push_back(tmp_obj);
        ranges_.push_back(objects.at(i).range);
        min_heights_.push_back(objects.at(i).min_height);
        max_heights_.push_back(objects.at(i).max_height);
      }
      detect_ready_ = true;
    }

    publish_if_possible();
    // ready_ = true;
  }
  /*void detections_callback(autoware_msgs::ImageObj image_objects_msg)
  {
          if (ready_)
                  return;
          ready_ = false;
          unsigned int num = image_objects_msg.obj.size();
          std::vector<autoware_msgs::ImageRect> objects =
  image_objects_msg.obj;
          //object_type = image_objects_msg.type;
          //points are X,Y,W,H and repeat for each instance
          obj_detections_.clear();
          tracked_type_ = image_objects_msg.type;
          for (unsigned int i=0; i<num;i++)
          {
                  cv::Rect tmp;
                  tmp.x = objects.at(i).x;
                  tmp.y = objects.at(i).y;
                  tmp.width = objects.at(i).width;
                  tmp.height = objects.at(i).height;
                  obj_detections_.push_back(ObjectDetection(tmp, 0));
          }
          ready_ = true;
  }*/

  void klt_config_cb() {}

  void Run() {
    std::string image_raw_topic_str;
    std::string image_obj_topic_str;

    ros::NodeHandle private_node_handle("~"); // to receive args

    if (private_node_handle.getParam("image_raw_node", image_raw_topic_str)) {
      ROS_INFO("Setting image node to %s", image_raw_topic_str.c_str());
    } else {
      ROS_INFO("No image node received, defaulting to /image_raw, you can use "
               "_image_raw_node:=YOUR_TOPIC");
      image_raw_topic_str = "/image_raw";
    }
    if (private_node_handle.getParam(ros::this_node::getNamespace() +
                                         "/img_obj_node",
                                     image_obj_topic_str)) {
      ROS_INFO("Setting object node to %s", image_obj_topic_str.c_str());
    } else {
      ROS_INFO("No object node received, defaulting to image_obj_ranged, you "
               "can use _img_obj_node:=YOUR_TOPIC");
      image_obj_topic_str = "image_obj_ranged";
    }

    publisher_tracked_objects_ =
        node_handle_.advertise<autoware_msgs::ImageObjTracked>(
            "image_obj_tracked", 1);

    ROS_INFO("Subscribing to... %s", image_raw_topic_str.c_str());
    ROS_INFO("Subscribing to... %s", image_obj_topic_str.c_str());
    subscriber_image_raw_ = node_handle_.subscribe(
        image_raw_topic_str, 1, &ROSTrackerApp::image_callback, this);
    subscriber_image_obj_ = node_handle_.subscribe(
        image_obj_topic_str, 1, &ROSTrackerApp::detections_callback, this);

    std::string config_topic("/config");
    config_topic += ros::this_node::getNamespace() + "/klt";
    // node_handle.subscribe(config_topic, 1, &ROSTrackerApp::klt_config_cb,
    // this);

    ros::spin();
    ROS_INFO("END klt");
  }

  ROSTrackerApp() {
    ready_ = true;
    num_trackers_ = 0;
    track_ready_ = false;
    detect_ready_ = false;
  }
};

int main(int argc, char *argv[]) {
  ros::init(argc, argv, "klt");

  ROSTrackerApp app;

  app.Run();

  return 0;
}
