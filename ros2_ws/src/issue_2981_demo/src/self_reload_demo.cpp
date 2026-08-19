#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp/parameter_map.hpp"

namespace
{
void print_params(const rclcpp::Node & node, const char * label)
{
  auto listed = node.list_parameters({}, 10);
  auto names = listed.names;
  std::sort(names.begin(), names.end());
  std::cout << "--- " << label << " ---\n";
  for (const auto & p : node.get_parameters(names)) {
    std::cout << "  " << p.get_name() << " = " << p.value_to_string() << "\n";
  }
}
}  // namespace

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  std::vector<std::string> args = rclcpp::remove_ros_arguments(argc, argv);
  if (args.size() < 2) {
    std::cerr << "usage: self_reload_demo <path/to/params.yaml>\n";
    rclcpp::shutdown();
    return 1;
  }
  const std::string yaml_path = args[1];

  // Same node identity and starting values as param_holder_node's
  // SprayerNode -- deliberately, so the "before" values printed below match
  // ../params/sprayer_params.yaml, same as every other demo in this package.
  auto node = std::make_shared<rclcpp::Node>("sprayer");
  node->declare_parameter("enabled", true);
  node->declare_parameter("nozzle_pressure_bar", 2.5);
  node->declare_parameter("spray_pattern", std::string("no"));
  node->declare_parameter(
    "active_zones", std::vector<std::string>{"zone_a", "zone_b", "zone_c"});
  node->declare_parameter("max_speed_mps", 1.8);
  node->declare_parameter("pass_count", 3);

  print_params(*node, "before (this process's own declare_parameter() defaults)");

  std::cout << "\nreloading " << yaml_path << " into this same node, in-process --"
    " no ros2 CLI, no parameter services, no second terminal\n\n";

  const auto map = rclcpp::parameter_map_from_yaml_file(
    yaml_path, node->get_fully_qualified_name());
  const auto params = rclcpp::parameters_from_map(map);
  const auto results = node->set_parameters(params);

  size_t failures = 0;
  for (const auto & result : results) {
    if (!result.successful) {
      std::cout << "  FAILED: " << result.reason << "\n";
      ++failures;
    }
  }
  std::cout << params.size() << " parameter(s) set, " << failures << " failure(s).\n\n";

  print_params(*node, "after (reloaded from file, same process, same node)");

  rclcpp::shutdown();
  return failures == 0 ? 0 : 1;
}
