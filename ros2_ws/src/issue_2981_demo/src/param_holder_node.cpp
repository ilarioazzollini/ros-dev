#include <string>
#include <vector>

#include "rclcpp/rclcpp.hpp"

class SprayerNode : public rclcpp::Node
{
public:
  SprayerNode()
  : Node("sprayer")
  {
    declare_parameter("enabled", true);
    declare_parameter("nozzle_pressure_bar", 2.5);
    // Deliberately YAML-ambiguous: unquoted `no` parses back as the boolean
    // `false`, not the string "no" -- a real serializer must quote it.
    declare_parameter("spray_pattern", std::string("no"));
    declare_parameter("active_zones", std::vector<std::string>{"zone_a", "zone_b", "zone_c"});
    declare_parameter("max_speed_mps", 1.8);
    declare_parameter("pass_count", 3);
  }
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<SprayerNode>();

  RCLCPP_INFO(
    node->get_logger(),
    "sprayer node up as '%s'. In another terminal try:\n"
    "  ros2 param dump %s\n"
    "  ros2 param load %s /root/ros-dev/ros2_ws/src/issue_2981_demo/"
    "params/sprayer_params_gentle.yaml   # switch config live, already works\n"
    "  ros2 param dump %s                # confirm it switched",
    node->get_fully_qualified_name(), node->get_fully_qualified_name(),
    node->get_fully_qualified_name(), node->get_fully_qualified_name());

  rclcpp::spin(node);

  rclcpp::shutdown();
  return 0;
}
