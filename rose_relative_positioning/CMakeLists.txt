cmake_minimum_required(VERSION 2.8.3)
project(rose_relative_positioning)

## Find catkin macros and libraries
## if COMPONENTS list like find_package(catkin REQUIRED COMPONENTS xyz)
## is used, also find other catkin packages
find_package(catkin REQUIRED COMPONENTS
            actionlib_msgs
            dynamic_reconfigure
            geometry_msgs
            operator_messaging
            roscomm
            roscpp
            rose_conversions
            rose_generic_platform
            rose_geometry
            rose_transformations
            server_multiple_client
            rose_shared_variables
            std_msgs
            tf
        )



add_action_files( DIRECTORY action 
    FILES 
    relative_positioning.action
)

generate_messages( DEPENDENCIES
    std_msgs 
    actionlib_msgs 
    geometry_msgs
    roscomm 
)

## Uncomment this if the package has a setup.py. This macro ensures
## modules and global scripts declared therein get installed
## See http://ros.org/doc/api/catkin/html/user_guide/setup_dot_py.html
# catkin_python_setup()

###################################
## catkin specific configuration ##
###################################
## The catkin_package macro generates cmake config files for your package
## Declare things to be passed to dependent projects
## INCLUDE_DIRS: uncomment this if you package contains header files
## LIBRARIES: libraries you create in this project that dependent projects also need
## CATKIN_DEPENDS: catkin_packages dependent projects also need
## DEPENDS: system dependencies of this project that dependent projects also need
catkin_package(
  INCLUDE_DIRS 
        include
  LIBRARIES
  CATKIN_DEPENDS  
        actionlib_msgs
        dynamic_reconfigure
        geometry_msgs
        operator_messaging
        roscomm
        roscpp
        rose_conversions
        rose_generic_platform
        rose_geometry
        rose_transformations
        server_multiple_client
        rose_shared_variables
        std_msgs
        tf
  DEPENDS
)

set(CMAKE_CXX_FLAGS "-std=c++0x -g") # TODO OH: -g is DEBUG FLAG

include_directories(
    include
    ${catkin_INCLUDE_DIRS}
)

add_executable( rose_relative_positioning
                src/relative_positioning.cpp
                src/relative_positioning_node.cpp
)

add_dependencies(rose_relative_positioning ${catkin_EXPORTED_TARGETS} ${PROJECT_NAME}_generate_messages_cpp)

target_link_libraries(rose_relative_positioning
    ${catkin_LIBRARIES}
)

