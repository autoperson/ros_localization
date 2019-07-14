#include "ros/ros.h"
// #include "time.h"
#include "std_msgs/String.h"
// ros_time
#include <ros/timer.h>
// ros_msg
#include <sensor_msgs/PointCloud2.h>
#include <geometry_msgs/Pose2D.h>

#include <nav_msgs/Odometry.h>
// pcl
#include <pcl/filters/voxel_grid.h>
#include <pcl/registration/icp.h>
#include <pcl_ros/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/registration/icp.h>
#include <pcl_conversions/pcl_conversions.h>
//#include <pcl/visualization/cloud_viewer.h>
#include <pcl/io/pcd_io.h>
//eigen
#include <Eigen/Dense>
//tf
#include <tf/transform_datatypes.h>
#include "Eigen/Core"
#include "Eigen/Geometry"
#include <tf/transform_broadcaster.h>
#include <tf_conversions/tf_eigen.h>
//nodelet
#include <nodelet/nodelet.h>
#include <pluginlib/class_list_macros.h>
//cpp
#include <ctime>
#include <limits.h>
#include <mutex>
//untility
#include <prm_localization/transform_utility.hpp>


namespace rt_localization_ns{
    using namespace std;


    class RealTime_Localization: public nodelet::Nodelet {
    public:
        RealTime_Localization(){
        }
        virtual  ~RealTime_Localization(){
        }

        void onInit() override {
            /**init**/
            nh = getNodeHandle();
            mt_nh = getMTNodeHandle();
            private_nh = getPrivateNodeHandle();
            /**parameter**/
            trim_low = private_nh.param<float>("trim_low", 0.0f);
            trim_high = private_nh.param<float>("trim_high", 4.0f);
            auto init_x = private_nh.param<float>("init_x", 0.0f);
            auto init_y = private_nh.param<float>("init_y", 0.0f);
            auto init_raw = private_nh.param<double>("init_raw", 0.0);
            auto icpMaximumIterations = private_nh.param<int>("icpMaximumIterations", 50);
            auto icpRANSACOutlierRejectionThreshold = private_nh.param<float>("icpRANSACOutlierRejectionThreshold", 0.04f);
            auto icpTransformationEpsilon = private_nh.param<float>("icpTransformationEpsilon", 1e-8);
            auto downSampleSize = private_nh.param<float>("downSampleSize", 0.05f);
            //init_pose
            curr_pose.setIdentity(4,4);
            curr_pose.block(0,0,3,3) = euler2rot(0,0,init_raw);
            curr_pose(0,3) =init_x;
            curr_pose(1,3) =init_y;
            curr_pose_stamp =ros::Time(0.001);
            //
            icp.setMaximumIterations(icpMaximumIterations);
            icp.setRANSACOutlierRejectionThreshold(icpRANSACOutlierRejectionThreshold);
            icp.setMaxCorrespondenceDistance(icpRANSACOutlierRejectionThreshold*100);
            icp.setTransformationEpsilon(icpTransformationEpsilon);
//        icp.set
            /**sub and pub**/
//        odom_pub = nh.advertise<nav_msgs::Odometry>("velodyne_link",50);
            points_suber = nh.subscribe("/velodyne_points",1,&RealTime_Localization::points_callback,this);
//            localmap_suber =  nh.subscribe("/localmap",1,&RealTime_Localization::localmap_callback,this);
//            curr_pointcloud_pub = nh.advertise<sensor_msgs::PointCloud2>("/registered_pointCloud",5);
//            points_suber = mt_nh.subscribe("/velodyne_points",1,&RealTime_Localization::points_callback,this);
            localmap_suber =  mt_nh.subscribe("/localmap",1,&RealTime_Localization::localmap_callback,this);
            curr_pointcloud_pub = mt_nh.advertise<sensor_msgs::PointCloud2>("/registered_pointCloud",5);
            /**utility param**/
            downSampler.setLeafSize(downSampleSize,downSampleSize,downSampleSize);

            NODELET_INFO("realTime_localization_nodelet initial completed");
        }

    private:

        void points_callback(const sensor_msgs::PointCloud2ConstPtr& points_msg){
            pcl::PointCloud<pcl::PointXYZ>::Ptr curr_cloud (new pcl::PointCloud<pcl::PointXYZ>());
            pcl::PointCloud<pcl::PointXYZ>::Ptr filtered_cloud (new pcl::PointCloud<pcl::PointXYZ>());
            pcl::fromROSMsg(*points_msg, *curr_cloud);
            //downsample
            downSampler.setInputCloud(curr_cloud);
            downSampler.filter(*filtered_cloud);
            //trim and 2d
            auto flat_cloud  = trimInputCloud(filtered_cloud, trim_low, trim_high);

            //icp
            icp.setInputTarget(localmap_cloud);
            icp.setInputSource(flat_cloud);
            pcl::PointCloud<pcl::PointXYZ> result_cloud ;
            //icp start
            clock_t start = clock();
            icp.align(result_cloud,curr_pose);//,curr_pose
            clock_t end = clock();
            NODELET_INFO("icp regis time = %f seconds",(double)(end  - start) / CLOCKS_PER_SEC);
//            NODELET_INFO("fitness score = %f ",icp.getFitnessScore());
//            NODELET_INFO(" ");
            Matrix4f transform = icp.getFinalTransformation();
//        Vector3f euler = rot2euler(transform.block(0,0,3,3));
//        NODELET_INFO("x = %f, y = %f, theta = %f ",transform(0,3),transform(1,3),euler(2));
            //
            {
                lock_guard<mutex> lockGuard(curr_pose_mutex);
                if(points_msg->header.stamp>curr_pose_stamp){
                    curr_pose=icp.getFinalTransformation();
                    NODELET_INFO("time to former stamp = %f seconds",points_msg->header.stamp.toSec()-curr_pose_stamp.toSec());
                    curr_pose_stamp = points_msg->header.stamp;

                } else {
                    NODELET_INFO("abandon former regis result");
                    return;
                }
            }

            //publish tf
            transformBroadcaster.sendTransform(matrix2transform(points_msg->header.stamp,curr_pose,"map","velodyne"));


            //publish cloud (optional)
            pcl::PointCloud<pcl::PointXYZRGB>::Ptr coloredCloud (new pcl::PointCloud<pcl::PointXYZRGB>());
            coloredCloud->width = result_cloud.width;
            coloredCloud->height = 1;//result_cloud.height
            coloredCloud->points.resize(coloredCloud->width*coloredCloud->height);

            for (size_t i=0;i<result_cloud.width;i++){
                coloredCloud->points[i].x = result_cloud.points[i].x;
                coloredCloud->points[i].y = result_cloud.points[i].y;
                coloredCloud->points[i].z = result_cloud.points[i].z;
                coloredCloud->points[i].r=255;
                coloredCloud->points[i].g=255;
            }
            pcl_conversions::toPCL(points_msg->header,coloredCloud->header);
            coloredCloud->header.frame_id = "map";
            curr_pointcloud_pub.publish(coloredCloud);



        }




        void localmap_callback(const sensor_msgs::PointCloud2ConstPtr& points_msg){
            localmap_cloud.reset(new pcl::PointCloud<pcl::PointXYZ>());
            pcl::fromROSMsg(*points_msg, *localmap_cloud);
            /** already contains stamp**/

//        const auto& stamp = points_msg->header.stamp;
//        pcl_conversions::toPCL(points_msg->header,localmap_cloud->header);

        }

        pcl::PointCloud<pcl::PointXYZ>::ConstPtr trimInputCloud(const pcl::PointCloud<pcl::PointXYZ>::ConstPtr &cloud,
                                                                const float low, const float high) const {
            pcl::PointCloud<pcl::PointXYZ>::Ptr cloudTrimmed (new pcl::PointCloud<pcl::PointXYZ>());
            for(size_t i=0;i<cloud->points.size();++i){
                if (cloud->points[i].z<high && cloud->points[i].z>low) {
                    pcl::PointXYZ currPoint;
                    currPoint.x =cloud->points[i].x;
                    currPoint.y =cloud->points[i].y;
                    //2d
//                currPoint.z =0;
                    //3d
                    currPoint.z =cloud->points[i].z;
                    cloudTrimmed->points.push_back(currPoint);
                }
            }
            cloudTrimmed->width = cloudTrimmed->points.size();
            cloudTrimmed->header = cloud->header;
            return cloudTrimmed;
        }

        Eigen::Matrix3d pc_register(const pcl::PointCloud<pcl::PointXY>::ConstPtr& curr_cloud,const pcl::PointCloud<pcl::PointXY>::ConstPtr& local_map,const Eigen::Matrix3d& initial_matrix){

        }

    private:

        //ros node handle
        ros::NodeHandle nh;
        ros::NodeHandle mt_nh;
        ros::NodeHandle private_nh;
        // para
        float trim_low;
        float trim_high;
        Eigen::Matrix4f curr_pose;
        ros::Time curr_pose_stamp;
        // suber and puber
        ros::Publisher curr_pointcloud_pub;
        ros::Subscriber odom_suber;
        ros::Subscriber points_suber;
        ros::Subscriber localmap_suber;
//    ros::Publisher odom_pub;
        ros::Timer timer;
        tf::TransformBroadcaster transformBroadcaster;
        // clouds
        pcl::PointCloud<pcl::PointXYZ>::Ptr localmap_cloud;

        // utility
        pcl::IterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> icp;
        pcl::VoxelGrid<pcl::PointXYZ> downSampler;
        std::mutex curr_pose_mutex;
        // time log
        ros::Duration full_time;
        double_t average_regis_time;
        int regis_num;


    };


}
PLUGINLIB_EXPORT_CLASS(rt_localization_ns::RealTime_Localization, nodelet::Nodelet)

//                NODELET_INFO("sec:%i,nsec:%i",points_msg->header.stamp.sec,points_msg->header.stamp.nsec);
//                NODELET_INFO("sec:%i,nsec:%i",curr_pose_stamp.sec,curr_pose_stamp.nsec);


//
//int main(int argc, char *argv[])
//{
//
//    //test code
////    pcl::PointCloud<pcl::PointXYZ>::Ptr mycloud (new pcl::PointCloud<pcl::PointXYZ>());
////    pcl::PointXYZ pointXy;
////    pointXy.x=2;
////    pointXy.y=2;
////
////    mycloud->points.push_back(pointXy);
////    //
//
//
//
//    ros::init(argc, argv, "rt_locator");
//    RealTime_Localization realtime_localization ;//(new RealTime_Localization())
//    realtime_localization.onInit();
//
//    ros::spin();
//
//    return 0;
//}
