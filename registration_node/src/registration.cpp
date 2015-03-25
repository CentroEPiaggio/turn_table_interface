// ROS headers
#include <ros/ros.h>
#include <ros/console.h>
#include <pcl_ros/point_cloud.h>
#include <std_srvs/Empty.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/point_cloud_conversion.h>
#include <pcl_conversions/pcl_conversions.h>

//ROS generated headers
#include "registration_node/set_poses.h"
#include "registration_node/execute.h"
#include "registration_node/save.h"
#include "registration_node/reconstruct.h"

//PCL headers
#include <pcl/registration/lum.h>
#include <pcl/registration/correspondence_estimation.h>
#include <pcl/registration/icp.h>
#include <pcl/filters/voxel_grid.h>
//#include <pcl/search/kdtree.h>
#include <pcl/common/eigen.h>
#include <pcl/io/pcd_io.h>
#include <pcl/io/vtk_lib_io.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/visualization/pcl_visualizer.h>
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/surface/gp3.h>
#include <pcl/features/normal_3d_omp.h>
#include <pcl/surface/mls.h>
#include <pcl/surface/impl/mls.hpp>

//general utilities
#include <cmath>
#include <fstream>
#include <string>
#include <stdlib.h>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/split.hpp>
//#include <boost/algorithm/string/trim.hpp>

#define D2R 0.017453293 //degrees to radians conversion

typedef pcl::PointXYZRGBA PT; //default point type
typedef pcl::PointCloud<PT> PC; //default point cloud
typedef pcl::PointNormal PNT; //default point normal type
typedef pcl::PointCloud<PNT> PCN; //default point cloud with normal
typedef std::pair<boost::filesystem::path, PC> pose;

using namespace boost::filesystem;

class register_poses
{
  public: 
    register_poses();
    ros::NodeHandle nh;
  private:
    ros::ServiceServer srv_set, srv_execute, srv_save, srv_reco;
    bool setPoses(registration_node::set_poses::Request& req, registration_node::set_poses::Response& res);
    bool execute(registration_node::execute::Request& req, registration_node::execute::Response& res);
    bool save(registration_node::save::Request& req, registration_node::save::Response& res);
    bool recon(registration_node::reconstruct::Request& req, registration_node::reconstruct::Response& res);
    
    std::vector<pose> original_poses;
    std::vector<pose> registered_poses;
    std::vector<PCN> smoothed_poses;

    PC::Ptr concatenated_original;
    PC::Ptr concatenated_registered;
    PCN::Ptr concatenated_smoothed;
    //PC::Ptr concatenated_registered_lum;
    
    pcl::PolygonMesh mesh;

    pcl::IterativeClosestPoint<PT,PT> icp;
    //pcl::registration::LUM<PT> lum;
    bool initialized, registered, reconstructed, smoothed;
    int i_30, i_70;
    //visualization
    pcl::visualization::PCLVisualizer viewer;
};

register_poses::register_poses()
{
  PC a,b;
  PCN c;
  concatenated_original= a.makeShared();
  concatenated_registered = b.makeShared();
  concatenated_smoothed = c.makeShared();
  //concatenated_registered_lum = c.makeShared();
  nh = ros::NodeHandle("registration_node");
  srv_set = nh.advertiseService("set_poses", &register_poses::setPoses, this);
  srv_execute = nh.advertiseService("exec_registration", &register_poses::execute, this);
  srv_save = nh.advertiseService("save_registered_poses", &register_poses::save, this);
  srv_reco = nh.advertiseService("reconstruct_surface", &register_poses::recon, this);
  initialized = false;
  registered = false;
  reconstructed = false;
  smoothed = false;
  //viewer.setRegistration(icp);
}

bool register_poses::setPoses (registration_node::set_poses::Request& req, registration_node::set_poses::Response& res)
{
  original_poses.clear();
  path directory (req.pose_dir);
  if ( !exists(directory) && !is_directory(directory) )
  {
    ROS_ERROR ("[Registration_Node] %s does not exists or it is not a directory! Aborting...\n", directory.string().c_str());
    return false;
  }
  std::vector<path> files;
  copy (directory_iterator(directory), directory_iterator(), back_inserter(files));
  sort (files.begin(), files.end(),
      [](path const &a, path const &b)
      {
        std::vector<std::string> va,vb;
        boost::split(va, a.stem().string(), boost::is_any_of("_"), boost::token_compress_on);
        boost::split(vb, b.stem().string(), boost::is_any_of("_"), boost::token_compress_on);
        int lat_a, lat_b, lon_a, lon_b;
        lat_a = std::stoi(va.at(va.size()-2));
        lat_b = std::stoi(vb.at(vb.size()-2));
        lon_a = std::stoi(va.at(va.size()-1));
        lon_b = std::stoi(vb.at(vb.size()-1));
        if (lat_a == 50 && lat_b != 50)
          return true;
        else if (lat_b == 50 && lat_a != 50)
          return false;
        else if (lat_a < lat_b)
          return true;
        else if (lat_a > lat_b)
          return false;
        else
        {
          if (lon_a < lon_b)
            return true;
          else
            return false;
        }
      });
  concatenated_original->clear();
  int i(0);
  for (std::vector<path>::const_iterator it (files.begin()); it != files.end(); ++it, ++i)
  {
    std::vector<std::string> vst;
    boost::split(vst, it->stem().string(), boost::is_any_of("_"), boost::token_compress_on);
    if (std::stoi(vst.at(vst.size()-2)) == 30 && std::stoi(vst.at(vst.size()-1)) == 0)
      i_30 = i;
    if (std::stoi(vst.at(vst.size()-2)) == 70 && std::stoi(vst.at(vst.size()-1)) == 0)
      i_70 = i;

    if (is_regular_file (*it) && extension (*it) == ".pcd")
    {
      pose tmp;
      pcl::io::loadPCDFile (it->c_str(), tmp.second);
      tmp.first = *it;
      original_poses.push_back(tmp);
      *concatenated_original += tmp.second;
    }
  }
  //LUM graph
  //let's suppose poses are acquired with poses_scanner, so they have three latitudes (30,50,70) we want 50_0 to be the reference pose
//  pcl::registration::CorrespondenceEstimation<PT,PT> corr;
//  int j(0); //track vertices ids
//  //cycle poses
//  for (i=0; i < original_poses.size(); ++i)
//  {   
//    lum.addPointCloud(original_poses[i].second.makeShared());
//    if (i==0)
//      continue;
//    if (i==i_30)
//    {
//      //save correspondences from 30_0 cloud to reference (50_0)
//      corr.setInputTarget(original_poses[0].second.makeShared());
//      corr.setInputSource(original_poses[i_30].second.makeShared());
//      pcl::CorrespondencesPtr to_50 (new pcl::Correspondences);
//      corr.determineCorrespondences(*to_50, 0.05);
//      lum.setCorrespondences(i_30, 0, to_50);
//      continue;
//    }
//    if (i==i_70)
//    {
//      //save correspondences from 70_0 cloud to reference (50_0)
//      corr.setInputTarget(original_poses[0].second.makeShared());
//      corr.setInputSource(original_poses[i_70].second.makeShared());
//      pcl::CorrespondencesPtr to_50 (new pcl::Correspondences);
//      corr.determineCorrespondences(*to_50, 0.05);
//      lum.setCorrespondences(i_70, 0, to_50);
//      continue;
//    }
//    //save correspondences from actual cloud to precedent
//    corr.setInputTarget(original_poses[i-1].second.makeShared());
//    corr.setInputSource(original_poses[i].second.makeShared());
//    pcl::CorrespondencesPtr actual_to_precedent (new pcl::Correspondences);
//    corr.determineCorrespondences(*actual_to_precedent, 0.05);
//    lum.setCorrespondences(i, i-1, actual_to_precedent);
//    if (i>i_30 && i<i_70)
//    {
//      //save correspondences from actual cloud to correspondent in 50
//      corr.setInputTarget(original_poses[i-i_30].second.makeShared());
//      corr.setInputSource(original_poses[i].second.makeShared());
//      pcl::CorrespondencesPtr actual_to_corr (new pcl::Correspondences);
//      corr.determineCorrespondences(*actual_to_corr, 0.05);
//      lum.setCorrespondences(i, i-i_30, actual_to_corr);
//    }
//    if (i>i_70)
//    {
//      //save correspondences from actual cloud to correspondent in 50
//      corr.setInputTarget(original_poses[i-i_70].second.makeShared());
//      corr.setInputSource(original_poses[i].second.makeShared());
//      pcl::CorrespondencesPtr actual_to_corr (new pcl::Correspondences);
//      corr.determineCorrespondences(*actual_to_corr, 0.05);
//      lum.setCorrespondences(i, i-i_70, actual_to_corr);
//    }
//    if (i==i_30-1)
//    {
//      //last element of 50 branch
//      //save correspondences from first to last
//      corr.setInputTarget(original_poses[i].second.makeShared());
//      corr.setInputSource(original_poses[0].second.makeShared());
//      pcl::CorrespondencesPtr first_to_last (new pcl::Correspondences);
//      corr.determineCorrespondences(*first_to_last, 0.05);
//      lum.setCorrespondences(0, i, first_to_last);
//    }
//    if (i==i_70-1)
//    {
//      //last element of 30 branch
//      //save correspondences from first to last
//      corr.setInputTarget(original_poses[i].second.makeShared());
//      corr.setInputSource(original_poses[i_30].second.makeShared());
//      pcl::CorrespondencesPtr first_to_last (new pcl::Correspondences);
//      corr.determineCorrespondences(*first_to_last, 0.05);
//      lum.setCorrespondences(i_30, i, first_to_last);
//    }
//    if (i==original_poses.size()-1)
//    {
//      //last element of 70 branch
//      //save correspondences from first to last
//      corr.setInputTarget(original_poses[i].second.makeShared());
//      corr.setInputSource(original_poses[i_70].second.makeShared());
//      pcl::CorrespondencesPtr first_to_last (new pcl::Correspondences);
//      corr.determineCorrespondences(*first_to_last, 0.05);
//      lum.setCorrespondences(i_70, i, first_to_last);
//    }
//  }
//  lum.setMaxIterations (1000);
//  lum.setConvergenceThreshold (0.0);
  ROS_INFO("[Registration_Node] %d poses loaded from %s.", (int)original_poses.size(), req.pose_dir.c_str());
  initialized = true;
  res.success = true;
  return true;
}

bool register_poses::execute(registration_node::execute::Request& req, registration_node::execute::Response& res)
{
  if (!initialized)
  {
    ROS_ERROR("[Registration_Node] No poses initialized for registration, call service `set_poses` first!!");
    return false;
  }
  ROS_WARN("[Registration_Node] Starting Registration procedure, PLEASE NOTE THAT IT COULD TAKE A LONG TIME...");
  concatenated_registered->clear();
//  concatenated_registered_lum->clear();
//  lum.compute(); //perform registration
//  *concatenated_registered_lum = *lum.getConcatenatedCloud();
//  for (int n=0; n<lum.getNumVertices(); ++n)
//  {
//    pose tmp;
//    tmp.first = original_poses[n].first;
//    tmp.second = *lum.getTransformedCloud(n);
//    //TODO adjust transformation stored in sensor origin
//    registered_poses_lum.push_back(tmp);
//  }
  //ICP
  icp.setTransformationEpsilon(1e-6);
  icp.setEuclideanFitnessEpsilon(1e-5);
  icp.setMaximumIterations(1000);
  icp.setUseReciprocalCorrespondences(true);
  icp.setInputTarget(original_poses[0].second.makeShared());
  registered_poses.push_back(original_poses[0]);
  PC::Ptr orig (original_poses[0].second.makeShared());
  PC::Ptr regis (registered_poses[0].second.makeShared());
  pcl::visualization::PointCloudColorHandlerCustom<PT> original_color (orig, 255,0,0);
  pcl::visualization::PointCloudColorHandlerCustom<PT> registered_color (regis, 0,255,0);
  viewer.addPointCloud(orig, original_color, "original");
  viewer.addPointCloud(regis, registered_color, "registered");
  viewer.addText("Original Pose in red", 50,20, 18, 1,0,0, "text1");
  viewer.addText("Registered Pose in green", 50,50, 18, 0,1,0, "text2");
  viewer.addText(original_poses[0].first.stem().c_str(), 30,80, 18, 0,0.4,0.9, "text3");
  int i=1;
  for (std::vector<pose>::iterator it(original_poses.begin()+1); it!=original_poses.end(); ++it,++i)
  {
    std::cout<<"\r"<<std::flush;
    viewer.spinOnce(100);
    if (i == i_30 || i == i_70)
    {
      icp.setInputTarget(registered_poses[0].second.makeShared());
    }
    else if (i>1)
    {
      icp.setInputTarget(registered_poses[i-1].second.makeShared());
    }
    icp.setInputSource(it->second.makeShared());
    pose reg;
    reg.first = it->first;
    std::cout<<"Registering (First Passage) "<<it->first.c_str()<<" \t ["<<i+1<<"/"<<original_poses.size()<<"]";
    icp.align(reg.second);
    //save new transformation in cloud sensor origin/orientation
    Eigen::Vector4f t_kli; 
    t_kli = it->second.sensor_origin_;
    Eigen::Matrix3f R_kli; 
    R_kli = it->second.sensor_orientation_;
    Eigen::Matrix4f T_kli, T_rli, T_lir, T_kr;
    T_kli << R_kli(0,0), R_kli(0,1), R_kli(0,2), t_kli(0),
             R_kli(1,0), R_kli(1,1), R_kli(1,2), t_kli(1),
             R_kli(2,0), R_kli(2,1), R_kli(2,2), t_kli(2),
             0,      0,      0,      1;
    T_rli = icp.getFinalTransformation();
    T_lir = T_rli.inverse();
    T_kr = T_kli * T_lir;
    Eigen::Matrix3f R_kr;
    R_kr = T_kr.topLeftCorner(3,3);
    Eigen::Quaternionf Q_kr (R_kr);
    Q_kr.normalize();
    Eigen::Vector4f t_kr(T_kr(0,3), T_kr(1,3), T_kr(2,3), 1);
    reg.second.sensor_origin_ = t_kr;
    reg.second.sensor_orientation_ = Q_kr;
    registered_poses.push_back(reg);
    pcl::copyPointCloud(it->second, *orig);
    pcl::copyPointCloud(reg.second, *regis);
    viewer.updatePointCloud(orig, original_color, "original");
    viewer.updatePointCloud(regis, registered_color, "registered");
    viewer.updateText(it->first.stem().c_str(), 30,80,18,0,0.4,0.9, "text3");
  }
  i=0;
  std::cout<<std::endl;
  for (std::vector<pose>::iterator it(registered_poses.begin()); it!=registered_poses.end(); ++it,++i)
  {
    std::cout<<"\r"<<std::flush;
    std::cout<<"Registering (Second Passage) "<<it->first.c_str()<<" \t ["<<i+1<<"/"<<original_poses.size()<<"]";    
    viewer.spinOnce(100);
    if (i < i_30)
      continue;

    if (i < i_70)
    {
      icp.setInputTarget(registered_poses[i - i_30].second.makeShared());
    }
    else if (i>= i_70 && i<original_poses.size())
    {
      icp.setInputTarget(registered_poses[i - i_70].second.makeShared());
    }
    icp.setInputSource(it->second.makeShared());
    PC::Ptr aligned (new PC);
    icp.align(*aligned);
    //save new transformation in cloud sensor origin/orientation
    Eigen::Vector4f t_kli; 
    t_kli = it->second.sensor_origin_;
    Eigen::Matrix3f R_kli; 
    R_kli = it->second.sensor_orientation_;
    Eigen::Matrix4f T_kli, T_rli, T_lir, T_kr;
    T_kli << R_kli(0,0), R_kli(0,1), R_kli(0,2), t_kli(0),
             R_kli(1,0), R_kli(1,1), R_kli(1,2), t_kli(1),
             R_kli(2,0), R_kli(2,1), R_kli(2,2), t_kli(2),
             0,      0,      0,      1;
    T_rli = icp.getFinalTransformation();
    T_lir = T_rli.inverse();
    T_kr = T_kli * T_lir;
    Eigen::Matrix3f R_kr;
    R_kr = T_kr.topLeftCorner(3,3);
    Eigen::Quaternionf Q_kr (R_kr);
    Q_kr.normalize();
    Eigen::Vector4f t_kr(T_kr(0,3), T_kr(1,3), T_kr(2,3), 1);
    aligned->sensor_origin_ = t_kr;
    aligned->sensor_orientation_ = Q_kr;
    pcl::copyPointCloud(*aligned, registered_poses[i].second);
    *concatenated_registered += *aligned;
    pcl::copyPointCloud(it->second, *orig);
    pcl::copyPointCloud(*aligned, *regis);
    viewer.updatePointCloud(orig, original_color, "original");
    viewer.updatePointCloud(regis, registered_color, "registered");
    viewer.updateText(it->first.stem().c_str(), 30,80,18,0,0.4,0.9, "text3");
  }
  viewer.removePointCloud("original");
  viewer.removePointCloud("registered");
  viewer.removeShape("text1");
  viewer.removeShape("text2");

  viewer.removeShape("text3");
  viewer.spinOnce(100);
  std::cout<<std::endl;
  i=0;
//  vg.setInputCloud(concatenated_registered_lum);
//  vg.filter(tmp);
//  pcl::copyPointCloud(tmp, *concatenated_registered_lum);
//  pcl::io::savePCDFile ( (home + "/original.pcd").c_str(), *concatenated_original );
//  pcl::io::savePCDFile ( (home + "/icp.pcd").c_str(), *concatenated_registered );
//  pcl::io::savePCDFile ( (home + "/lum.pcd").c_str(), *concatenated_registered_lum );
  ROS_INFO("[Registration_Node] Registration complete!");
  res.success = true;
  registered = true;
  return true;
}

bool register_poses::save(registration_node::save::Request& req, registration_node::save::Response& res)
{
  if (!registered)
  {
    ROS_ERROR("[Registration_Node] Registration is not done yet, call service `exec_registration` first!!");
    return false;
  }
  path directory (req.save_dir);
  if ( !exists(directory) && !is_directory(directory) )
  {
    create_directory(directory);
  }
  for (std::vector<pose>::const_iterator it (registered_poses.begin()); it != registered_poses.end(); ++it)
  {
    path old_file (it->first);
    path file (directory);
    file /= old_file.filename();
    pcl::io::savePCDFileBinaryCompressed (file.c_str(), it->second);  
  }
  path concatenated_reg (directory);
  path concatenated_orig (directory);
  path concatenated_smooth (directory);
  std::vector<std::string> vst;
  boost::split(vst, original_poses[0].first.stem().string(), boost::is_any_of("_"), boost::token_compress_on);
  path name_reg (vst.at(vst.size()-3) + "_reg.pcd");
  path name_orig (vst.at(vst.size()-3) + "_orig.pcd");
  path name_smooth (vst.at(vst.size()-3) + "_smooth.pcd");
  concatenated_reg /= name_reg;
  concatenated_orig /= name_orig;
  concatenated_smooth /= name_smooth;
  pcl::io::savePCDFileBinaryCompressed (concatenated_reg.c_str(), *concatenated_registered); 
  pcl::io::savePCDFileBinaryCompressed (concatenated_orig.c_str(), *concatenated_original); 
  if (smoothed)
    pcl::io::savePCDFileBinaryCompressed (concatenated_smooth.c_str(), *concatenated_smoothed); 
  res.success = true;
  ROS_INFO("[Registration_Node] Registered poses saved into %s", directory.c_str());
  return true;
}
bool register_poses::recon(registration_node::reconstruct::Request& req, registration_node::reconstruct::Response& res)
{
  if (!registered)
  {
    ROS_ERROR("[Registration_Node] Registration is not done yet, call service `exec_registration` first!!");
    return false;
  }
  ROS_WARN("[Registration_Node] Starting Surface Reconstruction procedure, PLEASE NOTE IT COULD TAKE A LONG TIME...");
  pcl::NormalEstimationOMP<pcl::PointXYZ,pcl::Normal> ne;
  pcl::search::KdTree<pcl::PointXYZ>::Ptr tree_o (new pcl::search::KdTree<pcl::PointXYZ>);
  int i(0);
  smoothed_poses.clear();
  concatenated_smoothed->clear();
  for (std::vector<pose>::iterator it(registered_poses.begin()); it!=registered_poses.end(); ++it,++i)
  {
    //dropping color and adding normals
    std::cout<<"\r"<<std::flush;
    std::cout<<"Computing normals of "<<it->first.c_str()<<" \t["<<i+1<<"/"<<registered_poses.size()<<"]";
    pcl::PointCloud<pcl::PointXYZ>::Ptr obj (new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::Normal>::Ptr normals (new pcl::PointCloud<pcl::Normal>);
    PCN::Ptr obj_with_normals (new PCN); 
    pcl::copyPointCloud(it->second, *obj);
    ne.setSearchMethod(tree_o);
    ne.setRadiusSearch(0.008);
    ne.setNumberOfThreads(0);
    ne.setInputCloud(obj);
    ne.useSensorOriginAsViewPoint();
    ne.compute(*normals);
    pcl::concatenateFields (*obj, *normals, *obj_with_normals);
    smoothed_poses.push_back(*obj_with_normals);
  }
  std::cout<<std::endl;
  
  PCN::Ptr temp (new PCN);
  pcl::search::KdTree<PNT>::Ptr tree (new pcl::search::KdTree<PNT>);
  pcl::VoxelGrid<PNT> vg;
  pcl::MovingLeastSquares<PNT, PNT> mls;
  pcl::RadiusOutlierRemoval<PNT> radout;
  
  PC::Ptr reg (new PC);
  pcl::PointCloud<pcl::PointXYZ>::Ptr smoo (new pcl::PointCloud<pcl::PointXYZ>);
  pcl::PointCloud<pcl::Normal>::Ptr smoo_n (new pcl::PointCloud<pcl::Normal>);
  pcl::visualization::PointCloudColorHandlerCustom<PT> registered_color (reg, 0,255,0);
  viewer.addPointCloud(reg, registered_color, "registered");
  viewer.addPointCloud(smoo, "smoothed");
  viewer.addText("Registered Pose in green", 50,20, 18, 0,1,0, "text1");
  viewer.addText("Smoothed Pose with normals", 50,50, 18, 0,0,1, "text2");

  for (int i=0; i<smoothed_poses.size(); ++i)
  {
    std::cout<<"\r"<<std::flush;
    viewer.removePointCloud("smoothed");
    std::cout<<"Smoothing: pre-downsample, MLS and filtering \t["<<i+1<<"/"<<smoothed_poses.size()<<"]"<<std::flush;
    vg.setLeafSize(0.001, 0.001, 0.001);
    vg.setInputCloud(smoothed_poses[i].makeShared());
    vg.setDownsampleAllData(true);
    vg.filter(*temp);
    pcl::copyPointCloud(*temp, smoothed_poses[i]);
    
    mls.setInputCloud(smoothed_poses[i].makeShared());
    mls.setSearchMethod (tree);
    mls.setUpsamplingMethod (pcl::MovingLeastSquares<PNT, PNT>::NONE);
    mls.setComputeNormals (false);
    mls.setPolynomialOrder (2);
    mls.setPolynomialFit (true);
    mls.setSearchRadius (0.01);
    mls.setSqrGaussParam (0.0001); //radius^2
    mls.process (*temp);
    pcl::copyPointCloud(*temp, smoothed_poses[i]);
    
    radout.setInputCloud(smoothed_poses[i].makeShared());
    radout.setRadiusSearch(0.006);
    radout.setMinNeighborsInRadius(20);
    radout.filter(*temp);
    pcl::copyPointCloud(*temp, smoothed_poses[i]);
    pcl::copyPointCloud(*temp, *smoo);
    pcl::copyPointCloud(*temp, *smoo_n);
    pcl::copyPointCloud(registered_poses[i].second, *reg);
    viewer.updatePointCloud(reg, "registered");
    viewer.addPointCloud(smoo, "smoothed");
    viewer.spinOnce(100);
    *concatenated_smoothed += smoothed_poses[i];
  }
  viewer.removePointCloud("registered");
  viewer.removePointCloud("smoothed");
  viewer.removeShape("text1");
  viewer.removeShape("text2");
  viewer.spinOnce(100);
  std::cout<<std::endl;
  vg.setLeafSize(0.0015, 0.0015, 0.0015);
  vg.setInputCloud(concatenated_smoothed);
  vg.filter(*temp);
  pcl::copyPointCloud(*temp, *concatenated_smoothed);
  //GreedyProjectionTriangulation
  pcl::GreedyProjectionTriangulation<PNT> gpt; 
  std::cout<<"Greedy Projection Triangulation..."<<std::flush;
  gpt.setSearchRadius(0.015);
  gpt.setMu(2.5);
  gpt.setMaximumNearestNeighbors (200);
  gpt.setMaximumSurfaceAngle(M_PI/3); 
  gpt.setMinimumAngle(M_PI/18); //5 deg
  gpt.setMaximumAngle(2*M_PI/3); //120 deg
  gpt.setNormalConsistency(true);

  gpt.setInputCloud(concatenated_smoothed);
  gpt.setSearchMethod(tree);
  gpt.reconstruct(mesh);
  
  smoothed=true;
  pcl::io::savePolygonFilePLY ("/home/tabjones/Desktop/mesh.ply", mesh);//TODO remove and include in save service
  std::cout<<"\tDone"<<std::endl;
  ROS_INFO("[Registration_Node] Surface reconstruction complete!");
  res.success = true;
}

int main (int argc, char *argv[])
{
  ros::init (argc, argv, "registration_node");
  register_poses registrationNode;
  ros::spin();
  return 0;
}
