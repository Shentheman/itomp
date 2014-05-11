#include <ros/ros.h>
#include <moveit/robot_state/robot_state.h>
#include <moveit_msgs/PlanningScene.h>
#include <itomp_cio_planner/optimization/evaluation_manager.h>
#include <itomp_cio_planner/model/itomp_planning_group.h>
#include <itomp_cio_planner/contact/ground_manager.h>
#include <itomp_cio_planner/visualization/visualization_manager.h>
#include <itomp_cio_planner/contact/contact_force_solver.h>
#include <itomp_cio_planner/util/min_jerk_trajectory.h>
#include <itomp_cio_planner/util/planning_parameters.h>
#include <itomp_cio_planner/util/vector_util.h>
#include <itomp_cio_planner/util/multivariate_gaussian.h>
#include <visualization_msgs/MarkerArray.h>
#include <geometric_shapes/mesh_operations.h>
#include <geometric_shapes/shape_operations.h>
#include <geometric_shapes/shapes.h>
#include <boost/variant/get.hpp>

using namespace std;
using namespace Eigen;

const static double SENSOR_NOISE = 0.18;

namespace itomp_cio_planner
{

EvaluationManager::EvaluationManager(int* iteration) :
    iteration_(iteration)
{

}

EvaluationManager::~EvaluationManager()
{

}

void EvaluationManager::initialize(ItompCIOTrajectory *full_trajectory, ItompCIOTrajectory *group_trajectory,
    ItompRobotModel *robot_model, const ItompPlanningGroup *planning_group, double planning_start_time,
    double trajectory_start_time)
{
  full_trajectory_ = full_trajectory;
  group_trajectory_ = group_trajectory;

  planning_start_time_ = planning_start_time;
  trajectory_start_time_ = trajectory_start_time;

  robot_model_ = robot_model;
  planning_group_ = planning_group;
  robot_name_ = robot_model_->getRobotName();

  kdl_joint_array_.resize(robot_model_->getKDLTree()->getNrOfJoints());

  // init some variables:
  num_joints_ = group_trajectory_->getNumJoints();
  num_contacts_ = group_trajectory_->getNumContacts();
  num_points_ = group_trajectory->getNumPoints();
  num_contact_points_ = group_trajectory->getNumContactPhases() + 1;

  // set up joint index:
  group_joint_to_kdl_joint_index_.resize(num_joints_);
  for (int i = 0; i < num_joints_; ++i)
    group_joint_to_kdl_joint_index_[i] = planning_group_->group_joints_[i].kdl_joint_index_;

  // set up the joint costs:
  joint_costs_.reserve(num_joints_);

  double max_cost_scale = 0.0;
  ros::NodeHandle nh("~");
  for (int i = 0; i < num_joints_; i++)
  {
    double joint_cost = 1.0;
    std::string joint_name = planning_group_->group_joints_[i].joint_name_;
    nh.param("joint_costs/" + joint_name, joint_cost, 1.0);
    std::vector<double> derivative_costs(NUM_DIFF_RULES);
    derivative_costs[DIFF_RULE_VELOCITY] = joint_cost * PlanningParameters::getInstance()->getSmoothnessCostVelocity();
    derivative_costs[DIFF_RULE_ACCELERATION] = joint_cost
        * PlanningParameters::getInstance()->getSmoothnessCostAcceleration();
    derivative_costs[DIFF_RULE_JERK] = joint_cost * PlanningParameters::getInstance()->getSmoothnessCostJerk();

    joint_costs_.push_back(
        SmoothnessCost(*group_trajectory_, i, derivative_costs, PlanningParameters::getInstance()->getRidgeFactor()));
    double cost_scale = joint_costs_[i].getMaxQuadCostInvValue();
    if (max_cost_scale < cost_scale)
      max_cost_scale = cost_scale;
  }

  // scale the smoothness costs
  for (int i = 0; i < num_joints_; i++)
  {
    joint_costs_[i].scale(max_cost_scale);
  }

  joint_axis_.resize(num_points_, std::vector<KDL::Vector>(robot_model_->getKDLTree()->getNrOfJoints()));
  joint_pos_.resize(num_points_, std::vector<KDL::Vector>(robot_model_->getKDLTree()->getNrOfJoints()));
  segment_frames_.resize(num_points_, std::vector<KDL::Frame>(robot_model_->getKDLTree()->getNrOfSegments()));

  is_collision_free_ = false;
  state_is_in_collision_.resize(num_points_);

  state_validity_.resize(num_points_);
  for (int i = 0; i < num_points_; ++i)
    state_validity_[i] = true;

  // Initialize visualizer
  VisualizationManager::getInstance()->setPlanningGroup(*robot_model_, planning_group_->name_);
  vis_marker_pub_ = VisualizationManager::getInstance()->getVisualizationMarkerPublisher();
  vis_marker_array_pub_ = VisualizationManager::getInstance()->getVisualizationMarkerArrayPublisher();

  last_trajectory_collision_free_ = false;
  dynamic_obstacle_cost_ = Eigen::VectorXd::Zero(num_points_);

  computeMassAndGravityForce();

  stateContactInvariantCost_.resize(num_points_);
  statePhysicsViolationCost_.resize(num_points_);
  stateCollisionCost_.resize(num_points_);

  GroundManager::getInstance().init();

  robot_model::RobotModelPtr ros_robot_model = robot_model->getRobotModel();
  planning_scene_.reset(new planning_scene::PlanningScene(ros_robot_model));
  initStaticEnvironment();

  costAccumulator_.addCost(TrajectoryCost::CreateTrajectoryCost(TrajectoryCost::COST_SMOOTHNESS));
  costAccumulator_.addCost(TrajectoryCost::CreateTrajectoryCost(TrajectoryCost::COST_COLLISION));
  costAccumulator_.addCost(TrajectoryCost::CreateTrajectoryCost(TrajectoryCost::COST_VALIDITY));
  costAccumulator_.addCost(TrajectoryCost::CreateTrajectoryCost(TrajectoryCost::COST_CONTACT_INVARIANT));
  costAccumulator_.addCost(TrajectoryCost::CreateTrajectoryCost(TrajectoryCost::COST_PHYSICS_VIOLATION));
  costAccumulator_.addCost(TrajectoryCost::CreateTrajectoryCost(TrajectoryCost::COST_GOAL_POSE));
  costAccumulator_.addCost(TrajectoryCost::CreateTrajectoryCost(TrajectoryCost::COST_COM));
  costAccumulator_.init(this);

  timings_.resize(20, 0);

}

void EvaluationManager::evaluateDerivatives(const Eigen::MatrixXd& parameters, const Eigen::MatrixXd& vel_parameters,
    const Eigen::MatrixXd& contact_parameters, Eigen::VectorXd& derivatives)
{
  // copy the parameters into group_trajectory_:
  int num_free_points = parameters.rows();
  ROS_ASSERT(group_trajectory_->getFreePoints().rows() == num_free_points);

  group_trajectory_->getFreePoints() = parameters;
  group_trajectory_->getFreeVelPoints() = vel_parameters;
  group_trajectory_->getContactTrajectory() = contact_parameters;

  group_trajectory_->updateTrajectoryFromFreePoints();

  // respect joint limits:
  handleJointLimits();

  // copy to full traj:
  updateFullTrajectory();

  // do forward kinematics:
  performForwardKinematics();

  computeTrajectoryValidity();
  computeWrenchSum();
  computeStabilityCosts();
  computeCollisionCosts();

  costAccumulator_.compute(this);
}

double EvaluationManager::evaluate()
{
  ros::Time time[10];
  time[0] = ros::Time::now();

  // do forward kinematics:
  last_trajectory_collision_free_ = performForwardKinematics();

  time[1] = ros::Time::now();
  timings_[1] += (time[1] - time[0]).toSec();

  computeTrajectoryValidity();
  last_trajectory_collision_free_ &= trajectory_validity_;

  time[2] = ros::Time::now();
  timings_[2] += (time[2] - time[1]).toSec();

  computeWrenchSum();

  time[3] = ros::Time::now();
  timings_[3] += (time[3] - time[2]).toSec();

  computeStabilityCosts();

  time[4] = ros::Time::now();
  timings_[4] += (time[4] - time[3]).toSec();

  computeCollisionCosts();

  time[5] = ros::Time::now();
  timings_[5] += (time[5] - time[4]).toSec();

  costAccumulator_.compute(this);

  time[6] = ros::Time::now();
  timings_[6] += (time[6] - time[5]).toSec();

  last_trajectory_collision_free_ &= costAccumulator_.isFeasible();

  // TODO: if trajectory is changed in handle joint limits,
  // update parameters

  time[7] = ros::Time::now();
  timings_[7] += (time[7] - time[6]).toSec();

  timings_[0] += (time[7] - time[0]).toSec();

  static int count = 0;
  if (++count % 1000 == 0)
  {
    PlanningParameters::getInstance()->initFromNodeHandle();
    VisualizationManager::getInstance()->render();

    costAccumulator_.print(*iteration_);
    printf("Contact Values :\n");
    for (int i = 0; i <= group_trajectory_->getNumContactPhases(); ++i)
    {
      printf("%d : ", i);
      for (int j = 0; j < group_trajectory_->getNumContacts(); ++j)
        printf("%f ", group_trajectory_->getContactValue(i, j));
      printf("   ");
      for (int j = 0; j < group_trajectory_->getNumContacts(); ++j)
      {
        KDL::Vector contact_position;
        planning_group_->contactPoints_[j].getPosition(i * group_trajectory_->getContactPhaseStride(), contact_position,
            segment_frames_);
        printf("%f ", contact_position.y());
      }
      printf("   ");
      for (int j = 0; j < group_trajectory_->getNumContacts(); ++j)
      {
        KDL::Vector contact_position;
        planning_group_->contactPoints_[j].getPosition(i * group_trajectory_->getContactPhaseStride(), contact_position,
            segment_frames_);
        printf("%f ", contact_position.z());
      }
      printf("\n");
    }

    printf("Elapsed Time : %f (Avg:%f)\n", timings_[0], timings_[0] / count);
    for (int i = 1; i <= 7; ++i)
    {
      printf("Elapsed Time %d : %f (Avg:%f) (%f\%)\n", i, timings_[i], timings_[i] / count,
          timings_[i] / timings_[0] * 100);
    }
  }

  return costAccumulator_.getTrajectoryCost();
}

double EvaluationManager::evaluate(const Eigen::MatrixXd& parameters, const Eigen::MatrixXd& vel_parameters,
    const Eigen::MatrixXd& contact_parameters, Eigen::VectorXd& costs)
{
  // copy the parameters into group_trajectory_:
  int num_free_points = parameters.rows();
  ROS_ASSERT(group_trajectory_->getFreePoints().rows() == num_free_points);

  group_trajectory_->getFreePoints() = parameters;
  group_trajectory_->getFreeVelPoints() = vel_parameters;
  group_trajectory_->getContactTrajectory() = contact_parameters;

  group_trajectory_->updateTrajectoryFromFreePoints();

  // respect joint limits:
  handleJointLimits();

  // copy to full traj:
  updateFullTrajectory();

  double cost = evaluate();

  ROS_ASSERT(costs.rows() == num_points_);
  for (int i = 0; i < num_points_; i++)
  {
    costs(i) = costAccumulator_.getWaypointCost(i);
  }

  return cost;
}

void EvaluationManager::render(int trajectory_index)
{
  if (PlanningParameters::getInstance()->getAnimateEndeffector())
  {
    VisualizationManager::getInstance()->animateEndeffector(trajectory_index, num_points_, 0, segment_frames_,
        state_validity_, false);
    VisualizationManager::getInstance()->animateCoM(num_points_, 0, CoMPositions_, false);
  }
  if (PlanningParameters::getInstance()->getAnimatePath())
  {
    VisualizationManager::getInstance()->animatePath(0, num_points_ - 1);
  }
}

void EvaluationManager::computeMassAndGravityForce()
{
  totalMass_ = 0.0;
  const KDL::SegmentMap& segmentMap = robot_model_->getKDLTree()->getSegments();
  numMassSegments_ = 0;
  for (KDL::SegmentMap::const_iterator it = segmentMap.begin(); it != segmentMap.end(); ++it)
  {
    const KDL::Segment& segment = it->second.segment;
    double mass = segment.getInertia().getMass();
    if (mass == 0)
      continue;

    totalMass_ += mass;
    masses_.push_back(mass);

    ++numMassSegments_;
  }
  gravityForce_ = totalMass_ * KDL::Vector(0.0, 0.0, -9.8);

  // normalize gravity force to 1.0
  gravityForce_ = KDL::Vector(0.0, 0.0, -1.0);

  linkPositions_.resize(numMassSegments_);
  linkVelocities_.resize(numMassSegments_);
  linkAngularVelocities_.resize(numMassSegments_);
  for (int i = 0; i < numMassSegments_; ++i)
  {
    linkPositions_[i].resize(num_points_);
    linkVelocities_[i].resize(num_points_);
    linkAngularVelocities_[i].resize(num_points_);
  }
  CoMPositions_.resize(num_points_);
  CoMVelocities_.resize(num_points_);
  CoMAccelerations_.resize(num_points_);
  CoMAccelerations_.resize(num_points_);
  AngularMomentums_.resize(num_points_);
  Torques_.resize(num_points_);
  wrenchSum_.resize(num_points_);

  int num_contacts = group_trajectory_->getNumContacts();
  contactViolationVector_.resize(num_contacts);
  contactPointVelVector_.resize(num_contacts);
  for (int i = 0; i < num_contacts; ++i)
  {
    contactViolationVector_[i].resize(num_points_);
    contactPointVelVector_[i].resize(num_points_);
  }
}

void EvaluationManager::handleJointLimits()
{
  /*
   for (int joint = 0; joint < num_joints_; joint++)
   {
   if (!planning_group_->group_joints_[joint].has_joint_limits_)
   continue;

   double joint_max = planning_group_->group_joints_[joint].joint_limit_max_;
   double joint_min = planning_group_->group_joints_[joint].joint_limit_min_;

   int count = 0;

   bool violation = false;
   do
   {
   double max_abs_violation = 1e-6;
   double max_violation = 0.0;
   int max_violation_index = 0;
   violation = false;
   for (int i = 1; i < num_points_ - 2; i++)
   {
   double amount = 0.0;
   double absolute_amount = 0.0;
   if ((*group_trajectory_)(i, joint) > joint_max)
   {
   amount = joint_max - (*group_trajectory_)(i, joint);
   absolute_amount = fabs(amount);
   }
   else if ((*group_trajectory_)(i, joint) < joint_min)
   {
   amount = joint_min - (*group_trajectory_)(i, joint);
   absolute_amount = fabs(amount);
   }
   if (absolute_amount > max_abs_violation)
   {
   max_abs_violation = absolute_amount;
   max_violation = amount;
   max_violation_index = i;
   violation = true;
   }
   }

   if (violation)
   {
   int free_var_index = max_violation_index - 1;
   double multiplier = max_violation
   / joint_costs_[joint].getQuadraticCostInverse()(free_var_index, free_var_index);
   group_trajectory_->getFreeJointTrajectoryBlock(joint) += multiplier
   * joint_costs_[joint].getQuadraticCostInverse().col(free_var_index);
   }
   if (++count > 10)
   break;
   } while (violation);
   }
   */
  for (int joint = 0; joint < num_joints_; joint++)
  {
    if (!planning_group_->group_joints_[joint].has_joint_limits_)
      continue;

    double joint_max = planning_group_->group_joints_[joint].joint_limit_max_;
    double joint_min = planning_group_->group_joints_[joint].joint_limit_min_;

    int count = 0;

    for (int i = 1; i < num_points_ - 2; i++)
    {
      if ((*group_trajectory_)(i, joint) > joint_max)
      {
        (*group_trajectory_)(i, joint) = joint_max;
      }
      else if ((*group_trajectory_)(i, joint) < joint_min)
      {
        (*group_trajectory_)(i, joint) = joint_min;
      }
    }
  }
}

void EvaluationManager::updateFullTrajectory()
{
  full_trajectory_->updateFromGroupTrajectory(*group_trajectory_);
}

bool EvaluationManager::performForwardKinematics()
{
  double invTime = 1.0 / group_trajectory_->getDiscretization();
  double invTimeSq = invTime * invTime;

  is_collision_free_ = true;

  // calculate the forward kinematics for the fixed states only in the first iteration:
  int start = 1;
  int end = num_points_ - 2;
  if (getIteration() <= 0)
  {
    start = 0;
    end = num_points_ - 1;

    // update segment_frames of the goal
    full_trajectory_->getTrajectoryPointKDL(end, kdl_joint_array_);
    planning_group_->fk_solver_->JntToCartFull(kdl_joint_array_, joint_pos_[end], joint_axis_[end],
        segment_frames_[end]);
  }

  // for each point in the trajectory
  for (int i = start; i <= end; ++i)
  {
    full_trajectory_->getTrajectoryPointKDL(i, kdl_joint_array_);
    // update kdl_joint_array with vel, acc
    if (i < 1)
    {
      for (int j = 0; j < planning_group_->num_joints_; j++)
      {
        int target_joint = planning_group_->group_joints_[j].kdl_joint_index_;
        kdl_joint_array_(target_joint) = (*group_trajectory_)(i, j);
      }
    }

    if (i == 0)
      planning_group_->fk_solver_->JntToCartFull(kdl_joint_array_, joint_pos_[i], joint_axis_[i], segment_frames_[i]);
    else
      planning_group_->fk_solver_->JntToCartPartial(kdl_joint_array_, joint_pos_[i], joint_axis_[i],
          segment_frames_[i]);

    state_is_in_collision_[i] = false;

    if (state_is_in_collision_[i])
    {
      is_collision_free_ = false;
    }
  }

  return is_collision_free_;
}

void EvaluationManager::computeTrajectoryValidity()
{
  trajectory_validity_ = true;
  const double clearance = 0.001;
  int collisionBV = 8001;
  visualization_msgs::Marker marker;
  visualization_msgs::Marker markerTemp;

  for (int i = 1; i < num_points_ - 1; i++)
  {
    bool valid = true;

    /*
     dynamic_obstacle_cost_(i) = 0;

     int full_traj_index = group_trajectory_->getFullTrajectoryIndex(i);
     full_trajectory_->getTrajectoryPointKDL(full_traj_index, kdl_joint_array_);
     for (int j = 0; j < full_trajectory_->getNumJoints(); ++j)
     {
     robot_state_.joint_state.position[j] = kdl_joint_array_(j);
     }

     planning_environment::setRobotStateAndComputeTransforms(robot_state_, *kinematic_state_);

     collision_space::EnvironmentModel
     * env_model =
     const_cast<collision_space::EnvironmentModel*> (collision_proximity_space_->getCollisionModelsInterface()->getOde());
     env_model->updateRobotModel(&(*kinematic_state_));

     // check collision points with dynamic obstacles
     double point_time = full_trajectory_->getDiscretization() * (i - 1);
     double cur_time = trajectory_start_time_ - planning_start_time_ + point_time;

     // TODO: dynamic obs
     double obstacleScale = 1.1 * (1 + cur_time * SENSOR_NOISE);
     obstacleScale = 1.0;

     bool inNextExecution = point_time < PlanningParameters::getInstance()->getPlanningStepSize();
     for (unsigned int j = 0; j < dynamic_obstacles_->size(); ++j)
     {
     const pomp_dynamic_obs_msgs::DynamicObstacle& dynamic_obstacle = dynamic_obstacles_->at(j);

     btVector3 origin(dynamic_obstacle.x, dynamic_obstacle.y, dynamic_obstacle.z);
     double obstacle_radius = dynamic_obstacle.lengthX * 0.5 * obstacleScale;
     }
     */

    state_validity_[i] = valid;
    if (!valid)
      trajectory_validity_ = false;

  }

}

void EvaluationManager::updateCoM(int point)
{
  const KDL::SegmentMap& segmentMap = robot_model_->getKDLTree()->getSegments();
  // compute CoM, p_j
  int massSegmentIndex = 0;
  CoMPositions_[point] = KDL::Vector::Zero();
  for (KDL::SegmentMap::const_iterator it = segmentMap.begin(); it != segmentMap.end(); ++it)
  {
    const KDL::Segment& segment = it->second.segment;
    double mass = segment.getInertia().getMass();
    int sn = robot_model_->getForwardKinematicsSolver()->segmentNameToIndex(segment.getName());
    const KDL::Vector& pos = segment_frames_[point][sn] * segment.getInertia().getCOG();
    if (mass == 0.0)
      continue;

    CoMPositions_[point] += pos * mass;
    linkPositions_[massSegmentIndex][point] = pos;
    ++massSegmentIndex;
  }
  CoMPositions_[point] = CoMPositions_[point] / totalMass_;
}

static bool STABILITY_COST_VERBOSE = false;

void EvaluationManager::computeWrenchSum()
{
  if (planning_group_->name_ != "lower_body" && planning_group_->name_ != "whole_body")
    return;

  int start = getIteration() == 0 ? 0 : 1;
  int end = getIteration() == 0 ? num_points_ - 1 : num_points_ - 2;

  // compute CoM, p_j
  for (int point = start; point <= end; ++point)
  {
    updateCoM(point);
  }

  // compute \dot{CoM} \ddot{CoM}
  itomp_cio_planner::getVectorVelocitiesAndAccelerations(1, num_points_ - 2, group_trajectory_->getDiscretization(),
      CoMPositions_, CoMVelocities_, CoMAccelerations_, KDL::Vector::Zero());
  // compute \dot{p_j}
  for (int i = 0; i < numMassSegments_; ++i)
  {
    itomp_cio_planner::getVectorVelocities(1, num_points_ - 2, group_trajectory_->getDiscretization(),
        linkPositions_[i], linkVelocities_[i], KDL::Vector::Zero());
  }

  // debug
  if (STABILITY_COST_VERBOSE)
  {
    printf("CoMPos CoMVel CoMAcc \n");
    for (int i = 1; i < num_points_ - 2; ++i)
    {
      printf("%f %f %f %f %f %f %f %f %f\n", CoMPositions_[i].x(), CoMPositions_[i].y(), CoMPositions_[i].z(),
          CoMVelocities_[i].x(), CoMVelocities_[i].y(), CoMVelocities_[i].z(), CoMAccelerations_[i].x(),
          CoMAccelerations_[i].y(), CoMAccelerations_[i].z());
    }
  }

  // TODO: compute angular velocities = (cur-prev)/time
  const KDL::SegmentMap& segmentMap = robot_model_->getKDLTree()->getSegments();
  const double invTime = 1.0 / group_trajectory_->getDiscretization();
  for (int point = 1; point <= num_points_ - 2; ++point)
  {
    int massSegmentIndex = 0;
    for (KDL::SegmentMap::const_iterator it = segmentMap.begin(); it != segmentMap.end(); ++it)
    {
      const KDL::Segment& segment = it->second.segment;
      double mass = segment.getInertia().getMass();
      int sn = robot_model_->getForwardKinematicsSolver()->segmentNameToIndex(segment.getName());
      const KDL::Vector& pos = segment_frames_[point][sn] * segment.getInertia().getCOG();
      if (mass == 0.0)
        continue;

      const KDL::Rotation& prevRotation = segment_frames_[point - 1][sn].M;
      const KDL::Rotation& curRotation = segment_frames_[point][sn].M;
      const KDL::Rotation& rotDiff = curRotation * prevRotation.Inverse();
      linkAngularVelocities_[massSegmentIndex][point] = rotDiff.GetRot() * invTime;
      ++massSegmentIndex;
    }
  }

  // compute angular momentum
  for (int point = 1; point <= num_points_ - 2; ++point)
  {
    AngularMomentums_[point] = KDL::Vector(0.0, 0.0, 0.0);

    int massSegmentIndex = 0;
    for (KDL::SegmentMap::const_iterator it = segmentMap.begin(); it != segmentMap.end(); ++it)
    {
      const KDL::Segment& segment = it->second.segment;
      double mass = segment.getInertia().getMass();
      if (mass == 0.0)
        continue;

      int sn = robot_model_->getForwardKinematicsSolver()->segmentNameToIndex(segment.getName());
      KDL::Vector angularVelTerm = (segment_frames_[point][sn] * segment.getInertia()).getRotationalInertia()
          * linkAngularVelocities_[massSegmentIndex][point];

      AngularMomentums_[point] += masses_[massSegmentIndex]
          * (linkPositions_[massSegmentIndex][point] - CoMPositions_[point]) * linkVelocities_[massSegmentIndex][point]
          + angularVelTerm;
      ++massSegmentIndex;
    }
  }
  // compute torques
  itomp_cio_planner::getVectorVelocities(1, num_points_ - 2, group_trajectory_->getDiscretization(), AngularMomentums_,
      Torques_, KDL::Vector::Zero());

  // compute wrench sum (gravity wrench + inertia wrench)
  for (int point = 1; point <= num_points_ - 2; ++point)
  {
    wrenchSum_[point].force = gravityForce_;
    wrenchSum_[point].torque = CoMPositions_[point] * gravityForce_;

    //wrenchSum_[point].force += -totalMass_ * CoMAccelerations_[point];
    //wrenchSum_[point].torque += -totalMass_ * CoMPositions_[point] * CoMAccelerations_[point] - Torques_[point];

    /*
     ROS_INFO("[%d] CoM pos:(%f %f %f)", point, CoMPositions_[point].x(), CoMPositions_[point].y(),
     CoMPositions_[point].z());
     ROS_INFO("[%d] CoM acc:(%f %f %f)", point, CoMAccelerations_[point].x(), CoMAccelerations_[point].y(),
     CoMAccelerations_[point].z());
     ROS_INFO("[%d] Ang mon:(%f %f %f)", point, AngularMomentums_[point].x(), AngularMomentums_[point].y(),
     AngularMomentums_[point].z());
     ROS_INFO("[%d] Com Tor:(%f %f %f)", point, Torques_[point].x(), Torques_[point].y(), Torques_[point].z());
     ROS_INFO("[%d] Wre For:(%f %f %f)", point, gravityForce_.x(), gravityForce_.y(), gravityForce_.z());
     ROS_INFO("[%d] Wre Tor:(%f %f %f)=(%f %f %f)x(%f %f %f)+%f(%f %f %f)x(%f %f %f)-(%f %f %f)", point,
     wrenchSum_[point].torque.x(), wrenchSum_[point].torque.y(), wrenchSum_[point].torque.z(),
     CoMPositions_[point].x(), CoMPositions_[point].y(), CoMPositions_[point].z(), gravityForce_.x(),
     gravityForce_.y(), gravityForce_.z(), totalMass_, CoMPositions_[point].x(), CoMPositions_[point].y(),
     CoMPositions_[point].z(), CoMAccelerations_[point].x(), CoMAccelerations_[point].y(),
     CoMAccelerations_[point].z(), Torques_[point].x(), Torques_[point].y(), Torques_[point].z());
     */

  }

  for (int i = 0; i < planning_group_->getNumContacts(); ++i)
  {
    planning_group_->contactPoints_[i].updateContactViolationVector(1, num_points_ - 2,
        group_trajectory_->getDiscretization(), contactViolationVector_[i], contactPointVelVector_[i], segment_frames_);
  }
}

void EvaluationManager::computeStabilityCosts()
{
  for (int point = 1; point <= num_points_ - 2; point++)
  {
    double state_contact_invariant_cost = 0.0;
    double state_physics_violation_cost = 0.0;
    if (planning_group_->name_ != "lower_body" && planning_group_->name_ != "whole_body")
    {
      stateContactInvariantCost_[point] = state_contact_invariant_cost;
      statePhysicsViolationCost_[point] = state_physics_violation_cost;
      continue;
    }

    int num_contacts = planning_group_->getNumContacts();
    std::vector<KDL::Vector> contact_forces(num_contacts);
    std::vector<KDL::Frame> contact_parent_frames(num_contacts);
    std::vector<double> contact_values(num_contacts);
    std::vector<KDL::Vector> contact_positions(num_contacts);
    for (int i = 0; i < num_contacts; ++i)
    {
      KDL::SegmentMap::const_iterator it_segment_link = robot_model_->getKDLTree()->getSegment(
          planning_group_->contactPoints_[i].getLinkName());
      it_segment_link = it_segment_link->second.parent;
      string parent_segment_name = it_segment_link->first;
      int segment_number = robot_model_->getForwardKinematicsSolver()->segmentNameToIndex(parent_segment_name);
      contact_parent_frames[i] = segment_frames_[point][segment_number];

      planning_group_->contactPoints_[i].getPosition(point, contact_positions[i], segment_frames_);
    }

    int phase = group_trajectory_->getContactPhase(point);
    for (int i = 0; i < num_contacts; ++i)
      contact_values[i] = group_trajectory_->getContactValue(phase, i);

    solveContactForces(PlanningParameters::getInstance()->getFrictionCoefficient(), contact_forces, contact_positions,
        wrenchSum_[point], contact_values, contact_parent_frames);

    for (int i = 0; i < num_contacts; ++i)
    {
      double cost = (contactViolationVector_[i][point].transpose() * contactViolationVector_[i][point]).value()
          + 16.0 * KDL::dot(contactPointVelVector_[i][point], contactPointVelVector_[i][point]);
      state_contact_invariant_cost += contact_values[i] * cost;
    }

    KDL::Wrench contactWrench;
    for (int i = 0; i < num_contacts; ++i)
    {
      contactWrench.force += contact_forces[i];
      contactWrench.torque += contact_positions[i] * contact_forces[i];
    }

    if (STABILITY_COST_VERBOSE)
    {
      printf("\n");

      KDL::Vector root_pos = segment_frames_[point][3].p;
      printf("%d Root : (%f %f %f) CoM : (%f %f %f)\n", point, root_pos.x(), root_pos.y(), root_pos.z(),
          CoMPositions_[point].x(), CoMPositions_[point].y(), CoMPositions_[point].z());
      for (int i = 0; i < num_contacts; ++i)
      {
        KDL::Vector rel_pos = (contact_positions[i] - CoMPositions_[point]);
        KDL::Vector contact_torque = rel_pos * contact_forces[i];
        printf("CP %d V:%f F:(%f %f %f) RT:(%f %f %f)xF=(%f %f %f) r:(%f %f %f) p:(%f %f %f)\n", i, contact_values[i],
            contact_forces[i].x(), contact_forces[0].y(), contact_forces[i].z(), rel_pos.x(), rel_pos.y(), rel_pos.z(),
            contact_torque.x(), contact_torque.y(), contact_torque.z(), contact_parent_frames[i].p.x(),
            contact_parent_frames[i].p.y(), contact_parent_frames[i].p.z(), contact_positions[i].x(),
            contact_positions[i].y(), contact_positions[i].z());
      }
    }

    KDL::Wrench violation = contactWrench + wrenchSum_[point];
    state_physics_violation_cost = sqrt(
        violation.force.x() * violation.force.x() + violation.force.y() * violation.force.y()
            + violation.force.z() * violation.force.z() + violation.torque.x() * violation.torque.x()
            + violation.torque.y() * violation.torque.y() + violation.torque.z() * violation.torque.z());

    if (STABILITY_COST_VERBOSE)
    {
      printf("Gravity Force : (%f %f %f)\n", gravityForce_.x(), gravityForce_.y(), gravityForce_.z());
      printf("Inertia Force : (%f %f %f)\n", -totalMass_ * CoMAccelerations_[point].x(),
          -totalMass_ * CoMAccelerations_[point].y(), -totalMass_ * CoMAccelerations_[point].z());

      printf("Wrench Torque : (%f %f %f)\n", wrenchSum_[point].torque.x(), wrenchSum_[point].torque.y(),
          wrenchSum_[point].torque.z());

      printf("Violation : (%f %f %f) (%f %f %f)\n", violation.force.x(), violation.force.y(), violation.force.z(),
          violation.torque.x(), violation.torque.y(), violation.torque.z());

      printf("[%d] contactWrench (%f %f %f)(%f %f %f)\n", point, contactWrench.force.x(), contactWrench.force.y(),
          contactWrench.force.z(), contactWrench.torque.x(), contactWrench.torque.y(), contactWrench.torque.z());
      printf("[%d] violation (%f %f %f)(%f %f %f)\n", point, violation.force.x(), violation.force.y(),
          violation.force.z(), violation.torque.x(), violation.torque.y(), violation.torque.z());

      printf("[%d]CIcost:%f Pvcost:%f(%f,%f,%f,%f,%f,%f)\n", point, state_contact_invariant_cost,
          state_physics_violation_cost, violation.force.x(), violation.force.y(), violation.force.z(),
          violation.torque.x(), violation.torque.y(), violation.torque.z());
    }

    stateContactInvariantCost_[point] = state_contact_invariant_cost;
    statePhysicsViolationCost_[point] = state_physics_violation_cost;
  }
}

void EvaluationManager::initStaticEnvironment()
{
  collision_detection::AllowedCollisionMatrix acm = planning_scene_->getAllowedCollisionMatrix();
  string environment_file = PlanningParameters::getInstance()->getEnvironmentModel();
  if (!environment_file.empty())
  {
    vector<double> environment_position = PlanningParameters::getInstance()->getEnvironmentModelPosition();
    double scale = PlanningParameters::getInstance()->getEnvironmentModelScale();
    environment_position.resize(3, 0);

    // Collision object
    moveit_msgs::CollisionObject collision_object;
    collision_object.header.frame_id = robot_model_->getRobotModel()->getModelFrame();
    collision_object.id = "environment";
    geometry_msgs::Pose pose;
    pose.position.x = environment_position[0];
    pose.position.y = environment_position[1];
    pose.position.z = environment_position[2];
    pose.orientation.x = sqrt(0.5);
    pose.orientation.y = 0.0;
    pose.orientation.z = 0.0;
    pose.orientation.w = sqrt(0.5);

    shapes::Mesh* shape = shapes::createMeshFromResource("package://move_itomp/meshes/" + environment_file);
    shapes::ShapeMsg mesh_msg;
    shapes::constructMsgFromShape(shape, mesh_msg);
    shape_msgs::Mesh mesh = boost::get<shape_msgs::Mesh>(mesh_msg);

    collision_object.meshes.push_back(mesh);
    collision_object.mesh_poses.push_back(pose);

    collision_object.operation = collision_object.ADD;
    moveit_msgs::PlanningScene planning_scene_msg;
    planning_scene_msg.world.collision_objects.push_back(collision_object);
    planning_scene_msg.is_diff = true;
    planning_scene_->setPlanningSceneDiffMsg(planning_scene_msg);

    acm.setEntry(true);
  }
}

void EvaluationManager::computeCollisionCosts()
{
  const collision_detection::AllowedCollisionMatrix acm = planning_scene_->getAllowedCollisionMatrix();

  collision_detection::CollisionRequest collision_request;
  collision_detection::CollisionResult collision_result;
  collision_request.verbose = false;
  collision_request.contacts = true;
  collision_request.max_contacts = 1000;

  robot_state::RobotStatePtr kinematic_state;
  std::vector<double> positions;

  kinematic_state.reset(new robot_state::RobotState(robot_model_->getRobotModel()));
  int num_all_joints = kinematic_state->getVariableCount();
  positions.resize(num_all_joints);

  for (int i = 0; i < num_points_; ++i)
  {
    double depthSum = 0.0;

    for (std::size_t k = 0; k < num_all_joints; k++)
    {
      positions[k] = (*full_trajectory_)(i, k);
    }
    kinematic_state->setVariablePositions(&positions[0]);
    //kinematic_state->update();

    planning_scene_->checkCollisionUnpadded(collision_request, collision_result, *kinematic_state, acm);
    const collision_detection::CollisionResult::ContactMap& contact_map = collision_result.contacts;
    for (collision_detection::CollisionResult::ContactMap::const_iterator it = contact_map.begin();
        it != contact_map.end(); ++it)
    {
      const collision_detection::Contact& contact = it->second[0];
      depthSum += contact.depth;

      /*
       Eigen::Vector3d pos = contact.pos;
       Eigen::Vector3d normal = contact.normal;
       printf("[%d] Depth : %f Pos : (%f %f %f) Normal : (%f %f %f)\n", i, contact.depth, pos(0), pos(1), pos(2), normal(0),
       normal(1), normal(2));
       */
    }
    collision_result.clear();
    stateCollisionCost_[i] = depthSum;
  }
}

void EvaluationManager::postprocess_ik()
{
  const double threshold = 0.1;
  const Eigen::MatrixXd& contactTrajectoryBlock = group_trajectory_->getContactTrajectory();
  int num_contact_phases = getGroupTrajectory()->getNumContactPhases();
  std::vector<int> ik_ref_point(num_contact_phases);
  for (int j = 0; j < num_contacts_; ++j)
  {
    for (int i = 0; i < num_contact_phases; ++i)
    {
      ik_ref_point[i] = -1;
    }

    for (int i = num_contact_phases - 1; i >= 0; --i)
    {
      double contact_value = contactTrajectoryBlock(i, j);
      if (contact_value > threshold)
      {
        ik_ref_point[i] = num_contact_phases;
      }
      else
        break;
    }
    int ref_point = 0;
    for (int i = 1; i < num_contact_phases; ++i)
    {
      double contact_value = contactTrajectoryBlock(i - 1, j);
      if (contact_value > threshold && ik_ref_point[i] != num_contact_phases)
        ik_ref_point[i] = ref_point;
      else
        ref_point = i;
    }

    /*
     printf("Contact %d : ", j);
     for (int i = 0; i < num_contact_phases; ++i)
     {
     if (ik_ref_point[i] != -1)
     printf("%d -> %d ", i, ik_ref_point[i]);
     }
     printf("\n");
     */

    ////////////
    // ik
    // TODO:
    string ik_group_name;
    switch (j)
    {
    case 0:
      ik_group_name = "left_leg";
      break;
    case 1:
      ik_group_name = "right_leg";
      break;
    case 2:
      ik_group_name = "left_arm";
      break;
    case 3:
      ik_group_name = "right_arm";
      break;
    }
    for (int i = 1; i < num_contact_phases; ++i)
    {
      KDL::Frame contact_frame;
      if (ik_ref_point[i] != -1)
      {
        planning_group_->contactPoints_[j].getFrame(ik_ref_point[i] * group_trajectory_->getContactPhaseStride(),
            contact_frame, segment_frames_);

        // set kinematic_state to current joint values
        robot_state::RobotStatePtr kinematic_state(new robot_state::RobotState(robot_model_->getRobotModel()));
        int num_all_joints = kinematic_state->getVariableCount();
        std::vector<double> positions(num_all_joints);
        for (std::size_t k = 0; k < num_all_joints; k++)
        {
          positions[k] = (*full_trajectory_)(i * group_trajectory_->getContactPhaseStride(), k);
        }
        kinematic_state->setVariablePositions(&positions[0]);
        kinematic_state->update();

        // compute ik for ref_point end effector position
        const robot_state::JointModelGroup* joint_model_group = robot_model_->getRobotModel()->getJointModelGroup(
            ik_group_name);

        Eigen::Affine3d end_effector_state = Eigen::Affine3d::Identity();
        for (int r = 0; r < 3; ++r)
          for (int c = 0; c < 3; ++c)
            end_effector_state(r, c) = contact_frame.M(r, c);
        for (int r = 0; r < 3; ++r)
          end_effector_state(r, 3) = contact_frame.p(r);

        bool found_ik = kinematic_state->setFromIK(joint_model_group, end_effector_state, 10, 0.1);
        // Now, we can print out the IK solution (if found):
        if (found_ik)
        {
          std::vector<double> group_values;
          kinematic_state->copyJointGroupPositions(joint_model_group, group_values);
          kinematic_state->setVariablePositions(&positions[0]);
          kinematic_state->setJointGroupPositions(joint_model_group, group_values);

          double* state_pos = kinematic_state->getVariablePositions();
          for (std::size_t k = 0; k < num_all_joints; k++)
          {
            (*full_trajectory_)(i * group_trajectory_->getContactPhaseStride(), k) = state_pos[k];
          }
          //ROS_INFO("[%d:%d] IK solution found", i, j);
        }
        else
        {
          //ROS_INFO("[%d:%d] Did not find IK solution", i, j);
        }
      }
    }
  }
  full_trajectory_->updateFreePointsFromTrajectory();
  group_trajectory_->copyFromFullTrajectory(*full_trajectory_);
  group_trajectory_->updateTrajectoryFromFreePoints();
}

double EvaluationManager::getTrajectoryCost(bool verbose)
{
  if (verbose)
    costAccumulator_.print(*iteration_);
  return costAccumulator_.getTrajectoryCost();
}

}
