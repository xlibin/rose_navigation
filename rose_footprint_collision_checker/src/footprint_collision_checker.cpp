/***********************************************************************************
* Copyright: Rose B.V. (2014)
*
* Revision History:
*   Author: Okke Hendriks
*   Date  : 2014/09/10
*       - File created.
*
* Description:
*   Given a number of points, a velocity twist message and a footprint. 
*   Calculate the distance to collision.
* 
***********************************************************************************/

#include "rose_footprint_collision_checker/footprint_collision_checker.hpp"

using namespace ClipperLib;

FootprintCollisionChecker::FootprintCollisionChecker()
    : max_distance_(3.0)
    , max_forward_sim_time_(5.0)
    , show_collissions_(false)
{
    loadParameters();

    rviz_marker_pub_ = n_.advertise<visualization_msgs::Marker>( "footprint_collision_checker_debug", 0 );

    timer = new boost::timer();
}

FootprintCollisionChecker::~FootprintCollisionChecker()
{}

void FootprintCollisionChecker::loadParameters()
{
    // Get a normal nodehandle to load the configurable parameters
    ros::NodeHandle n = ros::NodeHandle();

    ROS_INFO("Loading '%s' parameters.", ros::this_node::getName().c_str());

    XmlRpc::XmlRpcValue footprint;

    ROS_ASSERT_MSG(n.getParam("footprint", footprint), "Parameter footprint must be specified."); 
    ROS_ASSERT_MSG(footprint.getType() == XmlRpc::XmlRpcValue::TypeArray, "Parameter footprint is not of type 'XmlRpc::XmlRpcValue::TypeArray'.");

    for (int i = 0; i < footprint.size(); ++i) 
    {
        XmlRpc::XmlRpcValue vertex = footprint[i];
        ROS_ASSERT_MSG(vertex.getType() == XmlRpc::XmlRpcValue::TypeArray, "Footprint: Each entry should be a list of two doubles [x, y].");
        ROS_ASSERT_MSG(vertex.size() == 2, "Footprint: Each entry should be a list of two doubles [x, y].");
        ROS_ASSERT_MSG(vertex[0].getType() == XmlRpc::XmlRpcValue::TypeDouble, "Footprint: Each entry should be a list of two doubles [x, y].");
        ROS_ASSERT_MSG(vertex[1].getType() == XmlRpc::XmlRpcValue::TypeDouble, "Footprint: Each entry should be a list of two doubles [x, y].");

        footprint_.push_back(rose_geometry::Point(static_cast<double>(vertex[0]), static_cast<double>(vertex[1]), 0.0));
    }

    ROS_INFO("Loaded '%s' parameters.", ros::this_node::getName().c_str());
}

bool FootprintCollisionChecker::setFrameOfMotion(const geometry_msgs::PoseStamped& frame_of_motion)
{
    frame_of_motion_ = frame_of_motion;
    return true;
}

bool FootprintCollisionChecker::setFootprint(const std::vector<rose_geometry::Point>& new_footprint)
{
    footprint_ = new_footprint;
    return true;
}

std::string FootprintCollisionChecker::getFrameOfMotion()
{
    return frame_of_motion_.header.frame_id;
}

std::vector<rose_geometry::Point> FootprintCollisionChecker::getFootprint()
{
    return footprint_;
}

bool FootprintCollisionChecker::addPoints(const StampedVertices& new_lethal_points)
{
    // This lock is needed because the getFrameInFrame contains a tf_listener which has a asynchronous spinner thread, then if 
    // points are added/clear via an sensor callback method segmentation faults can/will happen.
    std::lock_guard<std::mutex> lock(points_mutex_);
    lethal_points_.insert( lethal_points_.end(), new_lethal_points.begin(), new_lethal_points.end() );
    
    // DEBUG
    // int id = 0;
    // for(const auto& stamped_lethal_point : lethal_points_)
    // {
    //     drawPoint(stamped_lethal_point, id++, 1.0, 0.0, 0.0);
    // }
    return true;
}

bool FootprintCollisionChecker::clearPoints()
{
    // This lock is needed because the getFrameInFrame contains a tf_listener which has a asynchronous spinner thread, then if 
    // points are added/clear via an sensor callback method segmentation faults can/will happen.
    std::lock_guard<std::mutex> lock(points_mutex_);
    lethal_points_.clear();
    return true;
}

StampedVertices FootprintCollisionChecker::transformPointsToFrame(const StampedVertices& stamped_points, const std::string& frame_id)
{
    // This lock is needed because the getFrameInFrame contains a tf_listener which has a asynchronous spinner thread, then if 
    // points are added/clear via an sensor callback method segmentation faults can/will happen.
    std::lock_guard<std::mutex> lock(points_mutex_);

    StampedVertices transformed_stamped_points;
    std::map<std::string, geometry_msgs::PoseStamped> transformations;
    
    ROS_DEBUG_NAMED(ROS_NAME, "Transforming %d vertices to frame '%s'.", (int)stamped_points.size(), frame_id.c_str());

    for(const StampedVertex& stamped_lethal_point : stamped_points)
    {
        std::string in_frame = stamped_lethal_point.header.frame_id;
        
        // Check if not yet in correct frame
        if(in_frame != frame_id)
        {
            geometry_msgs::PoseStamped transformation;

            // Do we already have this transform looked-up and stored in the map
            if(transformations.find(stamped_lethal_point.header.frame_id) != transformations.end()) 
            {
                // Load the transformation from the map
                ROS_DEBUG_NAMED(ROS_NAME, "Loading transformation from lethal point in frame '%s' to frame of motion '%s' from transformations map.", in_frame.c_str(), frame_id.c_str());
                transformation = transformations.at(stamped_lethal_point.header.frame_id);
            }
            else
            {
                ROS_DEBUG_NAMED(ROS_NAME, "Looking up transformation from lethal point in frame '%s' to frame of motion '%s'.", in_frame.c_str(), frame_id.c_str());
                // Lookup the transform and safe to the map
                if( not rose_transformations::getLatestFrameInFrame(tf_listener_, frame_id, stamped_lethal_point.header.frame_id, transformation) )  
                {
                    ROS_WARN_NAMED(ROS_NAME, "Error looking up transformation from lethal point in frame '%s' to frame of motion '%s'. Skipping this point.", in_frame.c_str(), frame_id.c_str());
                    continue;
                }

                // Add to map
                ROS_DEBUG_NAMED(ROS_NAME, "Adding transformation from '%s' -> '%s' to transformations map.", in_frame.c_str(), frame_id.c_str());
                transformations[stamped_lethal_point.header.frame_id] = transformation;
            }

            // Transform to frame_id
            StampedVertex transformed_stamped_lethal_point(stamped_lethal_point.header, stamped_lethal_point.data);
            transformed_stamped_lethal_point.header.frame_id = frame_id;

            rose_geometry::translatePoint(-transformation.pose.position.x, -transformation.pose.position.y, transformed_stamped_lethal_point.data);
            rose_geometry::rotatePointAroundOrigin(transformed_stamped_lethal_point.data, -tf::getYaw(transformation.pose.orientation));
            
            transformed_stamped_points.push_back(transformed_stamped_lethal_point);
        }
        else
            transformed_stamped_points.push_back(stamped_lethal_point);
    }

    return transformed_stamped_points;
}

// Return true if a collision does occur within forward_t
bool FootprintCollisionChecker::checkVelocity(const geometry_msgs::Twist& vel, const float& forward_t)
{
    return checkTrajectory(calculatePoseTrajectory(vel, 0.2, forward_t, max_distance_));
}

// Return true if a collision does occur
bool FootprintCollisionChecker::checkTrajectory(const Trajectory& trajectory)
{
    // timer = new boost::timer();
    // ROS_INFO("TIMING %s|%d: %2.10f", __FILE__, __LINE__, timer->elapsed());
    // ROS_INFO_NAMED(ROS_NAME, "checkTrajectory");

    if(footprint_.size() <= 2)
    {
        ROS_WARN_NAMED(ROS_NAME, "Footprint not set correctly. The footprint needs to consist out of at least three points.");
        return true;
    }
    publishPolygon(footprint_, frame_of_motion_.header.frame_id, "footprint");

    Path swept_polygon_path = getSweptPolygonPath(trajectory, footprint_);

    // publishPolygon(pathToPolygon(swept_polygon_path), frame_of_motion_.header.frame_id, "swept_polygon");
    // ROS_INFO("TIMING %s|%d: %2.10f", __FILE__, __LINE__, timer->elapsed());
    StampedVertices transformed_lethal_points = transformPointsToFrame(lethal_points_, frame_of_motion_.header.frame_id);   //! @todo OH [IMPR]: Transform the trajectory instead of the points.
    // ROS_INFO("TIMING %s|%d: %2.10f", __FILE__, __LINE__, timer->elapsed());
    bool collides = pathCollission(swept_polygon_path, transformed_lethal_points);
    // ROS_INFO("TIMING %s|%d: %2.10f", __FILE__, __LINE__, timer->elapsed());
      
    
    if(collides)
    {
        // ROS_INFO_NAMED(ROS_NAME, "Collision detected.");
        return true;
    }

    // ROS_INFO_NAMED(ROS_NAME, "Collision free travel for complete trajectory.");
    return false;
}

void FootprintCollisionChecker::getTrajectoryDistance(const Trajectory& trajectory, float& euclidean_distance, float& rotation)
{
    if(trajectory.size() < 2)
        return;

    float euclidean_distance_delta          = 0.0;
    float rotation_delta                    = 0.0;
    euclidean_distance                      = 0.0;
    rotation                                = 0.0;

    for(auto iter = std::next(trajectory.begin()); iter < trajectory.end(); iter++)
    {
        getPoseDistance(*std::prev(iter), *iter, euclidean_distance_delta, rotation_delta);
        
        euclidean_distance  += euclidean_distance_delta;
        rotation            += rotation_delta;
    }
}

void FootprintCollisionChecker::getPoseDistance(const geometry_msgs::PoseStamped& pose_a, const geometry_msgs::PoseStamped& pose_b, float& euclidean_distance, float& rotation)
{
    euclidean_distance  = rose_geometry::distanceXY(pose_a, pose_b);
    rotation            = rose_geometry::getShortestSignedAngle(tf::getYaw(pose_a.pose.orientation), tf::getYaw(pose_b.pose.orientation));
}

Polygon FootprintCollisionChecker::createAABB( const Polygon& polygon, float margin)
{
    float minx = 1e6;
    float maxx = -1e6;
    float miny = 1e6;
    float maxy = -1e6;
    for(const auto& point : polygon)
    {
        minx = fmin(point.x, minx);
        maxx = fmax(point.x, maxx);
        miny = fmin(point.y, miny);
        maxy = fmax(point.y, maxy);
    }

    minx -= margin;
    maxx += margin;
    miny -= margin;
    maxy += margin;

    vector<rose_geometry::Point> bounding_polygon;
    bounding_polygon.push_back(Vertex(maxx, maxy, 0.0)); 
    bounding_polygon.push_back(Vertex(minx, maxy, 0.0));
    bounding_polygon.push_back(Vertex(minx, miny, 0.0));
    bounding_polygon.push_back(Vertex(maxx, miny, 0.0));

    return bounding_polygon;
}

Polygon FootprintCollisionChecker::createAABB( const Path& path, float margin)
{
    float minx = 1e6;
    float maxx = -1e6;
    float miny = 1e6;
    float maxy = -1e6;
    for(const auto& point : path)
    {
        minx = fmin(point.X, minx);
        maxx = fmax(point.X, maxx);
        miny = fmin(point.Y, miny);
        maxy = fmax(point.Y, maxy);
    }

    minx -= margin;
    maxx += margin;
    miny -= margin;
    maxy += margin;

    vector<rose_geometry::Point> bounding_polygon;
    bounding_polygon.push_back(Vertex(maxx, maxy, 0.0)); 
    bounding_polygon.push_back(Vertex(minx, maxy, 0.0));
    bounding_polygon.push_back(Vertex(minx, miny, 0.0));
    bounding_polygon.push_back(Vertex(maxx, miny, 0.0));

    return bounding_polygon;
}

bool FootprintCollisionChecker::inAABB(const Vertex& point, const Polygon& aabb)
{
    return (point.x < aabb.at(0).x and point.x > aabb.at(2).x and point.y < aabb.at(0).y and point.y > aabb.at(2).y);
}

bool FootprintCollisionChecker::pathCollission(const Path& path, const StampedVertices& stamped_lethal_points)
{
    if(stamped_lethal_points.empty())
        return false;

    int id = 0;
    // ROS_INFO_NAMED(ROS_NAME, "FCC checking for collision using %d points and a polygon with %d vertices.", (int)stamped_lethal_points.size(), (int)path.size());
    Polygon aabb = createAABB(path, 0.001);
    for(const auto& stamped_lethal_point : stamped_lethal_points)
    {
        if(show_collissions_)
            drawPoint(stamped_lethal_point, id++, 1.0, 0.0, 0.0);

        // if(polyInAABB(polygon, 0.001, stamped_lethal_point.data))
        if(inAABB(stamped_lethal_point.data, aabb))
        {
            if(PointInPolygon(IntPoint(stamped_lethal_point.data.x*POLYGON_PRECISION, stamped_lethal_point.data.y*POLYGON_PRECISION), path))
                return true;
        }
    }

    return false;
}

bool FootprintCollisionChecker::polygonCollission(const Polygon& polygon, const StampedVertices& stamped_lethal_points)
{
    if(stamped_lethal_points.empty())
        return false;

    int id = 0;
    // ROS_INFO_NAMED(ROS_NAME, "FCC checking for collision using %d points and a polygon with %d vertices.", (int)stamped_lethal_points.size(), (int)path.size());
    Polygon aabb = createAABB(polygon, 0.001);
    for(const auto& stamped_lethal_point : stamped_lethal_points)
    {
        if(show_collissions_)
            drawPoint(stamped_lethal_point, id++, 1.0, 0.0, 0.0);

        if(inAABB(stamped_lethal_point.data, aabb))
        {
            Path path = polygonToPath(polygon);
            if(PointInPolygon(IntPoint(stamped_lethal_point.data.x*POLYGON_PRECISION, stamped_lethal_point.data.y*POLYGON_PRECISION), path))
                return true;
        }
    }

    return false;
}

Trajectory FootprintCollisionChecker::calculatePoseTrajectory(  const geometry_msgs::Twist& vel, 
                                                                const float& dt, 
                                                                const float& forward_t, 
                                                                const float& max_distance)
{
    float dyaw;
    geometry_msgs::Vector3 translation;
    translation.x   = dt*vel.linear.x;
    translation.y   = dt*vel.linear.y;
    dyaw            = dt*vel.angular.z;

    geometry_msgs::PoseStamped moving_pose = frame_of_motion_;

    ros::Time generation_time = ros::Time::now();
    
    float at_t          = 0.0;
    float at_distance   = 0.0;
    Trajectory trajectory;

    do
    {          
        moving_pose         = rose_geometry::translatePose(moving_pose, translation);
        tf::Quaternion quat = tf::createQuaternionFromYaw(dyaw);
        moving_pose         = rose_geometry::rotatePose(moving_pose, quat);

        translation         = rose_geometry::rotate2DVector(translation, dyaw);

        trajectory.push_back(moving_pose);
        at_t        += dt;
        at_distance += sqrt(translation.x*translation.x + translation.y*translation.y);
    } while (at_t <= forward_t && at_distance <= max_distance);

    // Debug trajectory
    // int id = 0;
    // ros::NodeHandle nh("~");
    // for(const auto& pose : trajectory)
    // {
    //     drawPose(nh, pose, id++, 0.0, 1.0, 0.0);
    // }

    return trajectory;
}

Polygon FootprintCollisionChecker::getPolygonAtPose(const geometry_msgs::PoseStamped& stamped_pose,  
                                                    const Polygon& polygon)
{
        Polygon transformed_polygon = polygon;
        
        // Rotate polygon around origin
        rose_geometry::rotatePointsAroundOrigin(tf::getYaw(stamped_pose.pose.orientation), transformed_polygon);
        // Translate polygon
        rose_geometry::translatePoints(stamped_pose.pose.position.x, stamped_pose.pose.position.y, transformed_polygon);      

        return transformed_polygon;
}

Path FootprintCollisionChecker::getSweptPolygonPath(const Trajectory& frame_of_motion_trajectory, const Polygon& polygon)
{
    return unionPaths(getSweptPolygonSubPaths(frame_of_motion_trajectory, polygon));
}

Polygon FootprintCollisionChecker::getSweptPolygonPolygon(const Trajectory& frame_of_motion_trajectory, const Polygon& polygon)
{
    return unionPolygons(getSweptPolygonSubPolys(frame_of_motion_trajectory, polygon));
}

Paths FootprintCollisionChecker::getSweptPolygonSubPaths(const Trajectory& frame_of_motion_trajectory, const Polygon& polygon)
{

    // Create a list of polygon's that will have to be union ed together later
    Paths swept_polygon_sub_paths;

    // Create a map to hold the path traced by all vertices in the polygon
    int vertex_path_id = 0;
    std::map<int, Path> vertex_paths;
    std::map<int, Path> closed_vertex_paths;

    // Loop trough all the poses of the frame_of_motion of the polygon
    for(const geometry_msgs::PoseStamped& stamped_pose : frame_of_motion_trajectory)
    {
        // Get the path at a pose of the trajectory
        Path path_at_pose = polygonToPath(getPolygonAtPose(stamped_pose, polygon));
        
        // Add this pose to the to be unioned polygons list
        swept_polygon_sub_paths.push_back(path_at_pose);

        // Add the vertexes of the path, at this pose of the trajectory, to their corresponding vertex path
        vertex_path_id = 0;
        for(const auto& vertex : path_at_pose)
            vertex_paths[vertex_path_id++].push_back(vertex); 
    }

    // For each vertex path, append the vertex path of the next vertex in the original polygon, in reverse
    for(vertex_path_id = 0; vertex_path_id < polygon.size() ; vertex_path_id++)
    {
        // Copy vertex path
        closed_vertex_paths[vertex_path_id] = vertex_paths.at(vertex_path_id);

        // Concatenate next vertex path in reverse
        int next_index = (vertex_path_id + 1) % polygon.size();
        closed_vertex_paths.at(vertex_path_id).insert   (   closed_vertex_paths.at(vertex_path_id).end(),
                                                            vertex_paths.at(next_index).rbegin(),
                                                            vertex_paths.at(next_index).rend() 
                                                        );

        ROS_DEBUG_NAMED(ROS_NAME, "Concatenated vertex paths %d and %d.", vertex_path_id, next_index);

        // Close the path by adding the starting vertex
        closed_vertex_paths.at(vertex_path_id).push_back(vertex_paths.at(vertex_path_id).front());

        // Cleanup self intersections etc.
        Paths simplified_vertex_path_paths;
        SimplifyPolygon(closed_vertex_paths.at(vertex_path_id), simplified_vertex_path_paths, pftNonZero);


        // Add the vertex path to the to be union-ed polygons list
        for(const auto& path : simplified_vertex_path_paths)
            swept_polygon_sub_paths.push_back(path);
    }

    return swept_polygon_sub_paths;
}

Polygons FootprintCollisionChecker::getSweptPolygonSubPolys(const Trajectory& frame_of_motion_trajectory, const Polygon& polygon)
{

    // Create a list of polygon's that will have to be union ed together later
    Polygons swept_polygon_sub_polys;

    // Create a map to hold the path traced by all vertices in the polygon
    int vertex_path_id = 0;
    std::map<int, Polygon> vertex_paths;
    std::map<int, Polygon> closed_vertex_paths;

    // Loop trough all the poses of the frame_of_motion of the polygon
    for(const geometry_msgs::PoseStamped& stamped_pose : frame_of_motion_trajectory)
    {
        // Get the polygon at a pose of the trajectory
        Polygon polygon_at_pose = getPolygonAtPose(stamped_pose, polygon);
        
        // Add this pose to the to be unioned polygons list
        swept_polygon_sub_polys.push_back(polygon_at_pose);

        // Add the vertexes of the polygon, at this pose of the trajectory, to their corresponding vertex path
        vertex_path_id = 0;
        for(const Vertex& vertex : polygon_at_pose)
            vertex_paths[vertex_path_id++].push_back(vertex); 
    }

    // For each vertex path, append the vertex path of the next vertex in the original polygon, in reverse
    for(vertex_path_id = 0; vertex_path_id < polygon.size() ; vertex_path_id++)
    {
        // Copy vertex path
        closed_vertex_paths[vertex_path_id] = vertex_paths.at(vertex_path_id);

        // Concatenate next vertex path in reverse
        int next_index = (vertex_path_id + 1) % polygon.size();
        closed_vertex_paths.at(vertex_path_id).insert   (   closed_vertex_paths.at(vertex_path_id).end(),
                                                            vertex_paths.at(next_index).rbegin(),
                                                            vertex_paths.at(next_index).rend() 
                                                        );

        ROS_DEBUG_NAMED(ROS_NAME, "Concatenated vertex paths %d and %d.", vertex_path_id, next_index);

        // Close the polygon by adding the starting vertex
        closed_vertex_paths.at(vertex_path_id).push_back(vertex_paths.at(vertex_path_id).front());

        // Cleanup self intersections etc.
        Path simplified_vertex_path_path = polygonToPath(closed_vertex_paths.at(vertex_path_id));
        Paths simplified_vertex_path_paths;
        SimplifyPolygon(simplified_vertex_path_path, simplified_vertex_path_paths, pftNonZero);
        Polygons simplified_vertex_path_polygons = pathsToPolygons(simplified_vertex_path_paths);


        for(const auto& polygon : simplified_vertex_path_polygons)
        {
            // Add the vertex path to the to be union-ed polygons list
            swept_polygon_sub_polys.push_back(polygon);

        }
    }

    return swept_polygon_sub_polys;
}

Path FootprintCollisionChecker::unionPaths(const Paths& paths)
{
    if(paths.size() == 1)
        return paths.front();

    Paths solution;
    Clipper clipper;
    clipper.AddPaths(paths, ptSubject, true);
    
    // Get the union
    clipper.Execute(ctUnion, solution, pftNonZero, pftNonZero);   

    if(solution.size() > 1)
        ROS_WARN_NAMED(ROS_NAME, "Union solution contains > 1 (%d) paths, continuing with first polygon. Consider a smaller timestep.", (int)solution.size());

    SimplifyPolygon(solution.front(), solution, pftNonZero);

    // Make sure all the solution paths are CCW
    clipper.Clear();
    bool reversed_a_path = false;
    for(auto path : solution)
    {
        if( not Orientation(path) )
        {
            ReversePath(path);
            reversed_a_path = true;
        }

        clipper.AddPath(path, ptSubject, true);
    }

    if(reversed_a_path)
    {
        ROS_DEBUG_NAMED(ROS_NAME, "Re-unioning polys to fix reversed paths.");
        solution.clear();
        clipper.Execute(ctUnion, solution, pftNonZero, pftNonZero);
    }

    if(solution.size() > 1)
        ROS_WARN_NAMED(ROS_NAME, "solution contains > 1 (%d) polygons, returning first polygon. Consider a smaller timestep.", (int)solution.size());

    return solution.front();
}

Polygon FootprintCollisionChecker::unionPolygons(const Polygons& polygons)
{
    return pathToPolygon(unionPaths(polygonsToPaths(polygons)));
}

Path FootprintCollisionChecker::trajectoryToPath(const Trajectory& trajectory)
{
    Path path;
    for(const auto& stamped_pose : trajectory)
        path.push_back( IntPoint((cInt)(stamped_pose.pose.position.x*POLYGON_PRECISION), (cInt)(stamped_pose.pose.position.y*POLYGON_PRECISION)) );
    return path;
}

Path FootprintCollisionChecker::polygonToPath(const Polygon& polygon)
{
    Path path;
    for(const Vertex& vertex : polygon)
        path.push_back( IntPoint((cInt)(vertex.x*POLYGON_PRECISION), (cInt)(vertex.y*POLYGON_PRECISION)) );
    return path;
}

Paths FootprintCollisionChecker::polygonsToPaths(const Polygons& polygons)
{
    Paths paths;
    for(const Polygon& polygon : polygons)
        paths.push_back(polygonToPath(polygon));
    return paths;
}

Polygon FootprintCollisionChecker::pathToPolygon(const Path& path)
{
    Polygon polygon;
    for(const auto& int_point : path)
        polygon.push_back( rose_geometry::Point((float)(int_point.X)/POLYGON_PRECISION, (float)(int_point.Y)/POLYGON_PRECISION, 0.0) );
    return polygon;
}

Polygons FootprintCollisionChecker::pathsToPolygons(const Paths& paths)
{
    Polygons polygons;
    for(const Path& path : paths)
        polygons.push_back(pathToPolygon(path));
    return polygons;
}

bool FootprintCollisionChecker::setMaxDistance(float max_distance)
{
    max_distance_ = max_distance;
    return true;
}

bool FootprintCollisionChecker::setMaxForwardSimTime(float max_forward_sim_time)
{
    max_forward_sim_time_ = max_forward_sim_time;
    return true;
}

void FootprintCollisionChecker::showCollisions()
{
    show_collissions_ = true;
}

void FootprintCollisionChecker::hideCollisions()
{
    show_collissions_ = false;
}

// --- debug ---

void FootprintCollisionChecker::drawPose(ros::NodeHandle& n, const geometry_msgs::PoseStamped& stamped_pose, int id, float r, float g, float b)
{
    geometry_msgs::Vector3 rpy = rose_conversions::quaternionToRPY(stamped_pose.pose.orientation);
    // ROS_INFO("drawPose, stamped_pose.header.frame_id: %s [%.3f, %.3f, %.3f, %.3f, %.3f, %.3f]", stamped_pose.header.frame_id.c_str(), stamped_pose.pose.position.x, stamped_pose.pose.position.y, stamped_pose.pose.position.z, rpy.x, rpy.y, rpy.z);

    visualization_msgs::Marker marker;
    marker.header.frame_id = stamped_pose.header.frame_id;
    marker.header.stamp = ros::Time::now();
    marker.ns = "poses";
    marker.id = 10000 + id;
    marker.type = visualization_msgs::Marker::ARROW;
    marker.action = visualization_msgs::Marker::ADD;
    marker.pose = stamped_pose.pose;
    marker.scale.x = 0.025;
    marker.scale.y = 0.01;
    marker.scale.z = 0.01;
    marker.color.r = r;
    marker.color.g = g;
    marker.color.b = b;
    marker.color.a = 0.75;
    marker.lifetime = ros::Duration(1.0);

    rviz_marker_pub_.publish(marker);
}

void FootprintCollisionChecker::publishPolygon(Polygon polygon, std::string frame, std::string name)
{
    // Check if publisher with this name already exists
    if ( footprint_pubs_.find(name) == footprint_pubs_.end() ) {
        // Not found, add it
        ros::Publisher footprint_pub;
        footprint_pubs_.insert(std::pair<std::string, ros::Publisher>(name, footprint_pub));
    
        if(name != "")
            footprint_pubs_.at(name)        = n_.advertise<geometry_msgs::PolygonStamped>( "debug/polygons/" + name, 1 );        
        else
            footprint_pubs_.at(name)        = n_.advertise<geometry_msgs::PolygonStamped>( "debug/polygons/default_name", 1 );                
    }   
    
    geometry_msgs::PolygonStamped footprint_polygon;
    footprint_polygon.header.frame_id   = frame;
    footprint_polygon.header.stamp      = ros::Time::now();

    for ( auto point : polygon )
    {
        Point32 point32;
        point32.x = point.x;
        point32.y = point.y;
        footprint_polygon.polygon.points.push_back(point32);
    }

    footprint_pubs_.at(name).publish( footprint_polygon );
}

void FootprintCollisionChecker::drawPoint(const StampedVertex& stamped_point, int id, float r, float g, float b)
{
    // ROS_INFO_NAMED(ROS_NAME, "drawPoint [%.3f, %.3f, %d] in frame '%s'", stamped_point.data.x, stamped_point.data.y, id, stamped_point.header.frame_id.c_str());

    visualization_msgs::Marker marker;
    marker.header.frame_id = stamped_point.header.frame_id;
    marker.header.stamp = ros::Time();
    marker.ns = "points";
    marker.id = id;
    marker.type = visualization_msgs::Marker::SPHERE;
    marker.action = visualization_msgs::Marker::ADD;
    marker.pose.position.x = stamped_point.data.x;
    marker.pose.position.y = stamped_point.data.y;
    marker.pose.position.z = 0.0;
    marker.pose.orientation.x = 0.0;
    marker.pose.orientation.y = 0.0;
    marker.pose.orientation.z = 0.0;
    marker.pose.orientation.w = 1.0;
    marker.scale.x = 0.02;
    marker.scale.y = 0.02;
    marker.scale.z = 0.02;
    marker.color.r = r;
    marker.color.g = g;
    marker.color.b = b;
    marker.color.a = 0.75;
    marker.lifetime = ros::Duration(1.0);

    rviz_marker_pub_.publish(marker);
}
