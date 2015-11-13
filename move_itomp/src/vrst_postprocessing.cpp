#include <pluginlib/class_loader.h>
#include <ros/ros.h>
#include <move_itomp/move_itomp_util.h>

#include <fstream>
#include <string>
#include <utility>

const double INV_SQRT_2 = 1.0 / std::sqrt((long double) 2.0);

using namespace move_itomp_util;


// from full_body_planner
void readTrajectory(moveit_msgs::DisplayTrajectory& display_trajectory, const std::string& filename, planning_scene::PlanningScenePtr& planning_scene,
                    std::vector<robot_state::RobotState>& robot_states)
{
    int start = robot_states.size();

    robot_state::RobotState state(planning_scene->getCurrentState());
    std::vector<robot_state::RobotState> robot_input_states;

    std::ifstream trajectory_file;
    trajectory_file.open(filename.c_str());
    if (trajectory_file.is_open())
    {
        std::string temp;
        for (int i = 0; i < 4; ++i)
            std::getline(trajectory_file, temp);

        int num_joints = state.getVariableCount();

        int num_lines = 0;
        while (trajectory_file >> temp)
        {
            if (temp == "Trajectory")
                break;

            trajectory_file >> temp;
            for (int j = 0; j < num_joints; ++j)
            {
                trajectory_file >> *(state.getVariablePositions() + j) ;
            }

            state.update(true);
            robot_input_states.push_back(state);

            ++num_lines;
        }

        trajectory_file.close();
    }

    int input_size = robot_input_states.size();
    for (int i = 0; i < input_size; ++i)
        robot_states.push_back(robot_input_states[i]);
    //robot_states.push_back(robot_input_states[0]);
    //robot_states.push_back(robot_input_states[input_size / 2]);

    // convert to moveit_msg
    moveit_msgs::MotionPlanResponse response;
    planning_interface::MotionPlanResponse res;
    res.error_code_.val = 1;

    res.trajectory_.reset(new robot_trajectory::RobotTrajectory(robot_states[0].getRobotModel(), ""));
    for (int i = start; i < robot_states.size(); ++i)
    {
        res.trajectory_->addSuffixWayPoint(robot_states[i], 0.05);
    }
    res.getMessage(response);

    display_trajectory.trajectory.push_back(response.trajectory);
}


void displayInitialWaypoints(std::vector<robot_state::RobotState>& states,
                             ros::NodeHandle& node_handle,
                             robot_model::RobotModelPtr& robot_model)
{
    static ros::Publisher vis_marker_array_publisher = node_handle.advertise<visualization_msgs::MarkerArray>("/move_itomp/visualization_marker_array", 10);

    visualization_msgs::MarkerArray ma;
    std::vector<std::string> link_names = robot_model->getLinkModelNames();
    std_msgs::ColorRGBA color;
    color.a = 0.5;
    color.r = 1.0;
    color.g = 1.0;
    color.b = 0.0;

    ros::Duration dur(3600.0);
    //ros::Duration dur(0.25);

    for (unsigned int point = 0; point < states.size(); ++point)
    {
        ma.markers.clear();


        const robot_state::RobotState& state = states[point];

        double time = point == 0 ? 2.00 : 0.05;
        ros::WallDuration timer(time);
        timer.sleep();


        std::string ns = "init_" + boost::lexical_cast<std::string>(point);
        state.getRobotMarkers(ma, link_names, color, ns, dur);
        vis_marker_array_publisher.publish(ma);
    }
}



moveit_msgs::Constraints setRootJointConstraint(moveit_msgs::Constraints& c,
                                                const robot_state::RobotState& robot_state)
{
    moveit_msgs::JointConstraint jc;

    const std::vector<std::string>& variable_names = robot_state.getVariableNames();
    const int variable_count = robot_state.getVariableCount();

    for (int i=0; i<variable_count; i++)
    {
        jc.joint_name = variable_names[i];
        jc.position = robot_state.getVariablePosition(variable_names[i]);
        c.joint_constraints.push_back(jc);
    }

    return c;
}


int main(int argc, char **argv)
{
    std::string benchmark_name = "";
    int motion = 0;
    int num_agents = 0;
    if (argc >= 2)
    {
        benchmark_name = argv[1];
        if (argc >= 3)
        {
            motion = atoi(argv[2]);
            if(argc >=4)
            {
                num_agents = atoi(argv[3]);
            }
        }
    }

	ros::init(argc, argv, "move_itomp");
	ros::AsyncSpinner spinner(1);
	spinner.start();
	ros::NodeHandle node_handle("~");

    robot_model_loader::RobotModelLoader robot_model_loader("robot_description");
	robot_model::RobotModelPtr robot_model = robot_model_loader.getModel();

    planning_scene::PlanningScenePtr planning_scene(new planning_scene::PlanningScene(robot_model));

	ros::Publisher planning_scene_diff_publisher;
    planning_scene_diff_publisher = node_handle.advertise<moveit_msgs::PlanningScene>("/planning_scene", 1);
	while (planning_scene_diff_publisher.getNumSubscribers() < 1)
	{
		ros::WallDuration sleep_t(0.5);
		sleep_t.sleep();
		ROS_INFO("Waiting planning_scene subscribers");
	}

    boost::scoped_ptr<pluginlib::ClassLoader<planning_interface::PlannerManager> > planner_plugin_loader;
    planning_interface::PlannerManagerPtr planner_instance;
    initializePlanner(planner_plugin_loader, planner_instance, node_handle, robot_model);

	loadStaticScene(node_handle, planning_scene, robot_model, planning_scene_diff_publisher);

	/* Sleep a little to allow time to startup rviz, etc. */
	ros::WallDuration sleep_time(1.0);
	sleep_time.sleep();

    ROS_INFO("Visualizing the trajectory");
    std::vector<ros::Publisher> display_publisher(num_agents);

    for (int agent = 0; agent < num_agents; agent++)
    {
        std::stringstream ss;
        ss << agent;
        display_publisher[agent] = node_handle.advertise<moveit_msgs::DisplayTrajectory>("/move_group/display_planned_path_" + ss.str(), 1, true);

        std::vector<std::string> trajectory_file;

        for (int i = 0; i <= motion; ++i)
        {
            std::stringstream ss;
            ss << "input/" << benchmark_name << "/"
               << "trajectory_out_" << std::setfill('0') << std::setw(4) << agent << "_" << std::setfill('0') << std::setw(4) << i << ".txt";
            std::string file_name = ss.str();

            trajectory_file.push_back(file_name);
        }

        // load trajectory
        moveit_msgs::DisplayTrajectory display_trajectory_input;
        moveit_msgs::DisplayTrajectory display_trajectory;

        std::vector<robot_state::RobotState> robot_states;
        for (int i = 0; i < trajectory_file.size(); ++i)
            readTrajectory(display_trajectory_input, trajectory_file[i], planning_scene, robot_states);

        /*
        std::vector<robot_state::RobotState> display_robot_states(robot_states.begin() + 140, robot_states.begin() + 160 + 21);
        displayInitialWaypoints(display_robot_states, node_handle, robot_model);
        */
        displayInitialWaypoints(robot_states, node_handle, robot_model);

        // set trajectory constraints
        unsigned int last = robot_states.size() - 40;
        for (unsigned int i = 0; i < last; i += 20)
        {
            planning_interface::MotionPlanRequest req;
            planning_interface::MotionPlanResponse res;

            for (unsigned int j = i; j <= i + 40; j ++)
            {
                moveit_msgs::Constraints constraint;
                setRootJointConstraint(constraint, robot_states[j]);

                if (j != i)
                {
                    const moveit_msgs::Constraints& prev_constraint = req.trajectory_constraints.constraints.back();
                    for (unsigned int k = 0; k < constraint.joint_constraints.size(); ++k)
                    {
                        double cur_pos = constraint.joint_constraints[k].position;
                        double prev_pos = prev_constraint.joint_constraints[k].position;
                        while (cur_pos -  prev_pos > M_PI + 0.001)
                            cur_pos -= 2 * M_PI;
                        while (cur_pos - prev_pos < -M_PI - 0.001)
                            cur_pos += 2 * M_PI;
                        constraint.joint_constraints[k].position = cur_pos;
                    }
                }

                req.trajectory_constraints.constraints.push_back(constraint);
            }

            // set the agent id and trajectory index
            ros::NodeHandle node_handle("itomp_planner");
            node_handle.setParam("agent_id", agent);
            node_handle.setParam("agent_trajectory_index", (int)i / 20);
            node_handle.setParam("benchmark_name", benchmark_name);

            displayStates(robot_states[i], robot_states[i + 40], node_handle, robot_model);
            doPlan("whole_body", req, res, robot_states[i], robot_states[i + 40], planning_scene, planner_instance);

            // make the start pose of next interpolation the end pose of current result            
            robot_states[i + 20] = res.trajectory_->getWayPoint(20);

            for (unsigned int j = 0; j < robot_states[0].getVariableCount(); ++j)
            {
                double cur_pos = robot_states[i + 20].getVariablePosition(j);
                double next_pos = robot_states[i + 21].getVariablePosition(j);
                while (next_pos - cur_pos > M_PI + 0.001)
                    cur_pos += 2 * M_PI;
                while (next_pos - cur_pos < -M_PI - 0.001)
                    cur_pos -= 2 * M_PI;
                robot_states[i + 20].setVariablePosition(j, cur_pos);
            }

            moveit_msgs::MotionPlanResponse response;
            res.getMessage(response);

            if (i == 0)
                display_trajectory.trajectory_start = response.trajectory_start;
            response.trajectory.joint_trajectory.points.resize(20);

            display_trajectory.trajectory.push_back(response.trajectory);
            display_publisher[agent].publish(display_trajectory);
            ros::WallDuration timer(0.1);
            timer.sleep();
        }

    }

    ROS_INFO("Done");

	return 0;
}
