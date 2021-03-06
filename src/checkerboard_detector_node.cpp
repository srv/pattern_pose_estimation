#include <ros/ros.h>
#include <tf/transform_broadcaster.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/Quaternion.h>
#include <image_transport/image_transport.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/image_encodings.h>
#include <opencv/cv.h>
#include <opencv/highgui.h>
#include <cv_bridge/cv_bridge.h>

#include <iostream>


class CheckerboardDetector
{
private:
  
  ros::NodeHandle nh_;
  ros::NodeHandle nh_priv_;
  image_transport::ImageTransport it_;
  image_transport::CameraSubscriber camera_sub_;
  ros::Publisher pose_pub_;
  tf::TransformBroadcaster tf_broadcaster_;
    
  std::vector<cv::Point3f> points3d_; // known 3D points

  bool rectified_;      // should be true if input image is rectified
  int cols_, rows_;     // number of internal internal corners
  bool show_detection_; // draw detection on screen
  std::string frame_id_; // id for the checkerboard's tf

  std::string window_name_;
  
public:

  CheckerboardDetector()
  : nh_(), nh_priv_("~"), it_(nh_)
  {
      init();
  }

  void init()
  {
    nh_priv_.param("cols", cols_, 8);
    nh_priv_.param("rows", rows_, 6);
    double square_size;
    nh_priv_.param("size", square_size, 0.06);
    nh_priv_.param("rectified", rectified_, true);
    nh_priv_.param("show_detection", show_detection_, false);
    nh_priv_.param("frame_id", frame_id_, std::string("checkerboard"));
    ROS_INFO_STREAM("Image is already rectified : " << rectified_);
    ROS_INFO_STREAM("Checkerboard parameters: " << rows_ << "x" << cols_ << ", size is " << square_size);
    float cx = square_size * cols_ / 2;
    float cy = square_size * rows_ / 2;
    // we define the world points in a way that cv::solvePnP directly gives us
    // the camera->checkerboard transformation as we want it:
    // center is at boards center, x points right (alignd with rows),
    // y points up (aligned with cols), z points out of the pattern.
    for (int i=0; i<rows_; i++)
    {
      for(int j=0; j<cols_; j++)
      {
        double x = j * square_size - cx;
        double y = cy - i * square_size;
        points3d_.push_back(cv::Point3f(x, y, 0.0));
      }
    }

    ROS_INFO_STREAM("Subscribing to image ropic " << nh_.resolveName("image"));
    camera_sub_ = it_.subscribeCamera("image", 1, &CheckerboardDetector::detect, this);

    pose_pub_ = nh_.advertise<geometry_msgs::PoseStamped>("checkerboard_pose", 1);

    window_name_ = "checkerboard detection - " + frame_id_;
    cv::namedWindow(window_name_, 0);
  }
  
  void sendMessageAndTransform(const cv::Mat& t_vec, const cv::Mat& r_vec, const ros::Time& stamp, const std::string& camera_frame_id)
  {
    tf::Vector3 axis(r_vec.at<double>(0, 0), r_vec.at<double>(1, 0), r_vec.at<double>(2, 0));
    double angle = cv::norm(r_vec);
    tf::Quaternion quaternion(axis, angle);
    
    tf::Vector3 translation(t_vec.at<double>(0, 0), t_vec.at<double>(1, 0), t_vec.at<double>(2, 0));

    tf::Transform transform(quaternion, translation);
    tf::StampedTransform stamped_transform(transform, stamp, camera_frame_id, frame_id_);
    tf_broadcaster_.sendTransform(stamped_transform);

    geometry_msgs::PoseStamped pose_msg;
    pose_msg.header.stamp = stamp;
    pose_msg.header.frame_id = camera_frame_id;
    tf::poseTFToMsg(transform, pose_msg.pose);

    pose_pub_.publish(pose_msg);
  }
  
  void detect(const sensor_msgs::ImageConstPtr& image, 
          const sensor_msgs::CameraInfoConstPtr& cam_info)
  { 
    cv_bridge::CvImageConstPtr cv_image = cv_bridge::toCvShare(image, sensor_msgs::image_encodings::MONO8);
    const cv::Mat& mat = cv_image->image;
    std::vector<cv::Point2f> corners;
    bool success = cv::findChessboardCorners(mat, cv::Size(cols_, rows_), corners,
        CV_CALIB_CB_ADAPTIVE_THRESH + CV_CALIB_CB_NORMALIZE_IMAGE + CV_CALIB_CB_FAST_CHECK);
    if (!success)
    {
      ROS_WARN_STREAM("Checkerboard not detected");
      if (show_detection_)
      {
           cv::imshow(window_name_, mat);
           cv::waitKey(5);
      }
      return;
    }
    cv::cornerSubPix(mat, corners, cv::Size(5,5), cv::Size(-1,-1), 
                     cv::TermCriteria(CV_TERMCRIT_EPS+CV_TERMCRIT_ITER, 30, 0.1 ));
    cv::Mat t_vec(3,1,CV_64FC1);
    cv::Mat r_vec(3,1,CV_64FC1);
    cv::Mat points_mat(points3d_);
    cv::Mat corners_mat(corners);
    if (rectified_)
    {
        const cv::Mat P(3,4, CV_64FC1, const_cast<double*>(cam_info->P.data()));
        // We have to take K' here extracted from P to take the R|t into account
        // that was performed during rectification.
        // This way we obtain the pattern pose with respect to the same frame that
        // is used in stereo depth calculation.
        const cv::Mat K_prime = P.colRange(cv::Range(0,3));
        cv::solvePnP(points_mat, corners_mat, K_prime, cv::Mat(), r_vec, t_vec);
    }
    else
    {
        const cv::Mat K(3,3, CV_64FC1, const_cast<double*>(cam_info->K.data()));
        const cv::Mat D(4,1, CV_64FC1, const_cast<double*>(cam_info->D.data()));
        cv::solvePnP(points_mat, corners_mat, K, D, r_vec, t_vec);
    }

    ros::Time stamp = image->header.stamp;
    if (stamp.toSec()==0.0)
      stamp = ros::Time::now();
    sendMessageAndTransform(t_vec, r_vec, stamp, image->header.frame_id);

    if (show_detection_)
    {
        cv::Mat draw;
        cv::cvtColor(mat, draw, CV_GRAY2BGR);
        cv::drawChessboardCorners(draw, cv::Size(cols_,rows_), corners, true);
        cv::imshow(window_name_, draw);
        cv::waitKey(5);
    }
  }
};

int main(int argc, char* argv[])
{
  ros::init(argc,argv,"checkerboard_detector");
  CheckerboardDetector cbd;
  ros::spin();
}
