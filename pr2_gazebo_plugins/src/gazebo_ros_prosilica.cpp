/*
 *  Gazebo - Outdoor Multi-Robot Simulator
 *  Copyright (C) 2003
 *     Nate Koenig & Andrew Howard
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
/*
 @mainpage
   Desc: GazeboRosProsilica plugin for simulating cameras in Gazebo
   Author: John Hsu
   Date: 24 Sept 2008
   SVN info: $Id$
 @htmlinclude manifest.html
 @b GazeboRosProsilica plugin mimics after prosilica_camera package
 */

#include <algorithm>
#include <assert.h>

#include <pr2_gazebo_plugins/gazebo_ros_prosilica.h>

#include "physics/World.hh"
#include "physics/HingeJoint.hh"
#include "sensors/Sensor.hh"
#include "sdf/interface/SDF.hh"
#include "sdf/interface/Param.hh"
#include "common/Exception.hh"
#include "sensors/CameraSensor.hh"
#include "sensors/SensorTypes.hh"
#include "rendering/Camera.hh"

#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/fill_image.h>
#include <diagnostic_updater/diagnostic_updater.h>
#include <sensor_msgs/RegionOfInterest.h>

#include <opencv/cv.h>
#include <opencv/highgui.h>

#include <cv.h>
#include <cvwimage.h>

#include <boost/scoped_ptr.hpp>
#include <boost/bind.hpp>
#include <boost/tokenizer.hpp>
#include <boost/thread.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <string>

namespace gazebo
{

////////////////////////////////////////////////////////////////////////////////
// Constructor
GazeboRosProsilica::GazeboRosProsilica()
{
}

////////////////////////////////////////////////////////////////////////////////
// Destructor
GazeboRosProsilica::~GazeboRosProsilica()
{
  // Finalize the controller
  this->poll_srv_.shutdown();
}

////////////////////////////////////////////////////////////////////////////////
// Load the controller
void GazeboRosProsilica::Load(sensors::SensorPtr _parent, sdf::ElementPtr _sdf)
{

  DepthCameraPlugin::Load(_parent, _sdf);
  this->parentSensor_ = this->parentSensor;
  this->width_ = this->width;
  this->height_ = this->height;
  this->depth_ = this->depth;
  this->format_ = this->format;
  this->camera_ = this->depthCamera;
  GazeboRosCameraUtils::Load(_parent, _sdf);

  // camera mode for prosilica:
  // prosilica::AcquisitionMode mode_; /// @todo Make this property of Camera

  //ROS_ERROR("before trigger_mode %s %s",this->mode_param_name.c_str(),this->mode_.c_str());

  if (!this->rosnode_->searchParam("trigger_mode",this->mode_param_name)) ///\@todo: hardcoded per prosilica_camera wiki api, make this an urdf parameter
      this->mode_param_name = "trigger_mode";

  if (!this->rosnode_->getParam(this->mode_param_name,this->mode_))
      this->mode_ = "streaming";

  ROS_INFO("trigger_mode %s %s",this->mode_param_name.c_str(),this->mode_.c_str());


  if (this->mode_ == "polled")
  {
      poll_srv_ = polled_camera::advertise(*this->rosnode_,this->pollServiceName,&GazeboRosProsilica::pollCallback,this);
  }
  else if (this->mode_ == "streaming")
  {
      ROS_DEBUG("do nothing here,mode: %s",this->mode_.c_str());
  }
  else
  {
      ROS_ERROR("trigger_mode is invalid: %s, using streaming mode",this->mode_.c_str());
  }
}

////////////////////////////////////////////////////////////////////////////////
// Update the controller
void GazeboRosProsilica::OnNewImageFrame(const unsigned char *_image, 
    unsigned int _width, unsigned int _height, unsigned int _depth, 
    const std::string &_format)
{
  if (!this->rosnode_->getParam(this->mode_param_name,this->mode_))
      this->mode_ = "streaming";

  // should do nothing except turning camera on/off, as we are using service.
  /// @todo: consider adding thumbnailing feature here if subscribed.
  common::Time sensor_update_time = this->parentSensor_->GetLastUpdateTime();

  // as long as ros is connected, parent is active
  //ROS_ERROR("debug image count %d",this->image_connect_count_);
  if (!this->parentSensor->IsActive())
  {
    if (this->image_connect_count_ > 0)
      // do this first so there's chance for sensor to run 1 frame after activate
      this->parentSensor->SetActive(true);
  }
  else
  {
    //ROS_ERROR("camera_ new frame %s %s",this->parentSensor_->GetName().c_str(),this->frame_name_.c_str());

    if (this->mode_ == "streaming")
    {
      if (this->image_connect_count_ > 0)
      {
        common::Time cur_time = this->world_->GetSimTime();
        if (cur_time - this->last_update_time_ >= this->update_period_)
        {
          this->PutCameraData(_image, sensor_update_time);
          this->last_update_time_ = cur_time;
        }
      }
    }

  }
  /// publish CameraInfo
  if (this->info_connect_count_ > 0)
  {
    common::Time cur_time = this->world_->GetSimTime();
    if (cur_time - this->last_info_update_time_ >= this->update_period_)
    {
      this->PublishCameraInfo(sensor_update_time);
      this->last_info_update_time_ = cur_time;
    }
  }
}


////////////////////////////////////////////////////////////////////////////////
// new prosilica interface.
void GazeboRosProsilica::pollCallback(polled_camera::GetPolledImage::Request& req,
                                      polled_camera::GetPolledImage::Response& rsp,
                                      sensor_msgs::Image& image, sensor_msgs::CameraInfo& info)
{
  if (!this->rosnode_->getParam(this->mode_param_name,this->mode_))
      this->mode_ = "streaming";

  /// @todo Support binning (maybe just cv::resize)
  /// @todo Don't adjust K, P for ROI, set CameraInfo.roi fields instead
  /// @todo D parameter order is k1, k2, t1, t2, k3

  if (this->mode_ != "polled")
  {
    rsp.success = false;
    rsp.status_message = "Camera is not in triggered mode";
    return;
  }

  if (req.binning_x > 1 || req.binning_y > 1)
  {
    rsp.success = false;
    rsp.status_message = "Gazebo Prosilica plugin does not support binning";
    return;
  }

  // get region from request
  if (req.roi.x_offset <= 0 || req.roi.y_offset <= 0 || req.roi.width <= 0 || req.roi.height <= 0)
  {
    req.roi.x_offset = 0;
    req.roi.y_offset = 0;
    req.roi.width = this->width_;
    req.roi.height = this->height;
  }
  const unsigned char *src = NULL;
  ROS_ERROR("roidebug %d %d %d %d", req.roi.x_offset, req.roi.y_offset, req.roi.width, req.roi.height);

  // signal sensor to start update
  this->image_connect_count_++;

  // wait until an image has been returned
  while(!src)
  {
    {
      // Get a pointer to image data
      src = this->parentSensor->GetDepthCamera()->GetImageData(0);

      if (src)
      {

        // fill CameraInfo
        this->roiCameraInfoMsg = &info;
        this->roiCameraInfoMsg->header.frame_id = this->frame_name_;

        common::Time roiLastRenderTime = this->parentSensor_->GetLastUpdateTime();
        this->roiCameraInfoMsg->header.stamp.sec = roiLastRenderTime.sec;
        this->roiCameraInfoMsg->header.stamp.nsec = roiLastRenderTime.nsec;

        this->roiCameraInfoMsg->width  = req.roi.width; //this->parentSensor->GetImageWidth() ;
        this->roiCameraInfoMsg->height = req.roi.height; //this->parentSensor->GetImageHeight();
        // distortion
#if ROS_VERSION_MINIMUM(1, 3, 0)
        this->roiCameraInfoMsg->distortion_model = "plumb_bob";
        this->roiCameraInfoMsg->D.resize(5);
#endif
        this->roiCameraInfoMsg->D[0] = this->distortion_k1_;
        this->roiCameraInfoMsg->D[1] = this->distortion_k2_;
        this->roiCameraInfoMsg->D[2] = this->distortion_k3_;
        this->roiCameraInfoMsg->D[3] = this->distortion_t1_;
        this->roiCameraInfoMsg->D[4] = this->distortion_t2_;
        // original camera matrix
        this->roiCameraInfoMsg->K[0] = this->focal_length_;
        this->roiCameraInfoMsg->K[1] = 0.0;
        this->roiCameraInfoMsg->K[2] = this->cx_ - req.roi.x_offset;
        this->roiCameraInfoMsg->K[3] = 0.0;
        this->roiCameraInfoMsg->K[4] = this->focal_length_;
        this->roiCameraInfoMsg->K[5] = this->cy_ - req.roi.y_offset;
        this->roiCameraInfoMsg->K[6] = 0.0;
        this->roiCameraInfoMsg->K[7] = 0.0;
        this->roiCameraInfoMsg->K[8] = 1.0;
        // rectification
        this->roiCameraInfoMsg->R[0] = 1.0;
        this->roiCameraInfoMsg->R[1] = 0.0;
        this->roiCameraInfoMsg->R[2] = 0.0;
        this->roiCameraInfoMsg->R[3] = 0.0;
        this->roiCameraInfoMsg->R[4] = 1.0;
        this->roiCameraInfoMsg->R[5] = 0.0;
        this->roiCameraInfoMsg->R[6] = 0.0;
        this->roiCameraInfoMsg->R[7] = 0.0;
        this->roiCameraInfoMsg->R[8] = 1.0;
        // camera projection matrix (same as camera matrix due to lack of distortion/rectification) (is this generated?)
        this->roiCameraInfoMsg->P[0] = this->focal_length_;
        this->roiCameraInfoMsg->P[1] = 0.0;
        this->roiCameraInfoMsg->P[2] = this->cx_ - req.roi.x_offset;
        this->roiCameraInfoMsg->P[3] = -this->focal_length_ * this->hack_baseline_;
        this->roiCameraInfoMsg->P[4] = 0.0;
        this->roiCameraInfoMsg->P[5] = this->focal_length_;
        this->roiCameraInfoMsg->P[6] = this->cy_ - req.roi.y_offset;
        this->roiCameraInfoMsg->P[7] = 0.0;
        this->roiCameraInfoMsg->P[8] = 0.0;
        this->roiCameraInfoMsg->P[9] = 0.0;
        this->roiCameraInfoMsg->P[10] = 1.0;
        this->roiCameraInfoMsg->P[11] = 0.0;
        this->camera_info_pub_.publish(*this->roiCameraInfoMsg);

        // copy data into image_msg_, then convert to roiImageMsg(image)
        this->image_msg_.header.frame_id    = this->frame_name_;

        common::Time lastRenderTime = this->parentSensor_->GetLastUpdateTime();
        this->image_msg_.header.stamp.sec = lastRenderTime.sec;
        this->image_msg_.header.stamp.nsec = lastRenderTime.nsec;

        //unsigned char dst[this->width_*this->height];

        /// @todo: don't bother if there are no subscribers

        // copy from src to image_msg_
        fillImage(this->image_msg_,
                  this->type_,
                  this->height_,
                  this->width_,
                  this->skip_*this->width_,
                  (void*)src );

        /// @todo: publish to ros, thumbnails and rect image in the Update call?

        this->image_pub_.publish(this->image_msg_);

        {
          // copy data into ROI image
          this->roiImageMsg = &image;
          this->roiImageMsg->header.frame_id = this->frame_name_;
          common::Time roiLastRenderTime = this->parentSensor_->GetLastUpdateTime();
          this->roiImageMsg->header.stamp.sec = roiLastRenderTime.sec;
          this->roiImageMsg->header.stamp.nsec = roiLastRenderTime.nsec;

          // convert image_msg_ to a CvImage using cv_bridge
          boost::shared_ptr<cv_bridge::CvImage> img_bridge_;
          img_bridge_ = cv_bridge::toCvCopy(this->image_msg_);

          // for debug
          //cvNamedWindow("showme",CV_WINDOW_AUTOSIZE);
          //cvSetMouseCallback("showme", &GazeboRosProsilica::mouse_cb, this);
          //cvStartWindowThread();
          //cvShowImage("showme",img_bridge_.toIpl());

          // crop image to roi
          cv::Mat roi(img_bridge_->image,
            cv::Rect(req.roi.x_offset, req.roi.y_offset,
                     req.roi.width, req.roi.height));
          img_bridge_->image = roi;

          // copy roi'd image into roiImageMsg
          img_bridge_->toImageMsg(*this->roiImageMsg);
        }
      }
    }
    usleep(100000);
  }
  this->image_connect_count_--;
  rsp.success = true;
  return;
}


/*
void GazeboRosProsilica::OnStats( const boost::shared_ptr<msgs::WorldStatistics const> &_msg)
{
  this->simTime  = msgs::Convert( _msg->sim_time() );

  math::Pose pose;
  pose.pos.x = 0.5*sin(0.01*this->simTime.Double());
  gzdbg << "plugin simTime [" << this->simTime.Double() << "] update pose [" << pose.pos.x << "]\n";
}
*/

// Register this plugin with the simulator
GZ_REGISTER_SENSOR_PLUGIN(GazeboRosProsilica)


}
