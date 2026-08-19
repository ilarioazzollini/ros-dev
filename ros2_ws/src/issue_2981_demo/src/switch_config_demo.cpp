#include <chrono>
#include <iostream>
#include <string>

#include "rclcpp/rclcpp.hpp"

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);

  // rclcpp::init() strips --ros-args; remaining argv are our positional args.
  std::vector<std::string> args = rclcpp::remove_ros_arguments(argc, argv);
  if (args.size() < 3) {
    std::cerr << "usage: switch_config_demo <target_node_fqn> <path/to/params.yaml>\n";
    rclcpp::shutdown();
    return 1;
  }
  const std::string target_node = args[1];
  const std::string yaml_path = args[2];

  auto owner_node = std::make_shared<rclcpp::Node>("switch_config_demo");
  auto client = std::make_shared<rclcpp::SyncParametersClient>(owner_node, target_node);

  if (!client->wait_for_service(std::chrono::seconds(5))) {
    std::cerr << "parameter services on " << target_node << " not available\n";
    rclcpp::shutdown();
    return 1;
  }

  std::cout << "loading " << yaml_path << " into " << target_node <<
    " -- in-process C++ (rclcpp::SyncParametersClient::load_parameters), "
    "no ros2 CLI involved\n";

  const auto results = client->load_parameters(yaml_path);

  size_t failures = 0;
  for (const auto & result : results) {
    if (!result.successful) {
      std::cout << "  FAILED: " << result.reason << "\n";
      ++failures;
    }
  }
  std::cout << results.size() << " parameter(s) set, " << failures << " failure(s).\n";

  rclcpp::shutdown();
  return failures == 0 ? 0 : 1;
}
