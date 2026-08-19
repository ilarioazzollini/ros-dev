#include <exception>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp/parameter_map.hpp"

namespace
{
constexpr char kSavePathParam[] = "save_path";
}  // namespace

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
    declare_parameter(kSavePathParam, std::string("sprayer_params_saved.yaml"));
  }
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<SprayerNode>();

  RCLCPP_INFO(
    node->get_logger(),
    "sprayer node up as '%s'. Ctrl-C here to save its parameters with "
    "rclcpp::serialize_parameters() -- the new capability #2981 adds.",
    node->get_fully_qualified_name());

  rclcpp::spin(node);

  std::string yaml;
  try {
    yaml = rclcpp::serialize_parameters(
      node->get_node_parameters_interface(), node->get_node_base_interface());
  } catch (const std::exception & e) {
    std::cerr << "\nrclcpp::serialize_parameters() failed: " << e.what() << "\n";
    rclcpp::shutdown();
    return 1;
  }
  std::cout << "\n--- serialized by rclcpp::serialize_parameters() "
    "(not hand-rolled) ---\n" << yaml;

  std::string path;
  node->get_parameter(kSavePathParam, path);
  std::ofstream file(path);
  if (!file) {
    std::cerr << "\ncould not open " << path << " for writing\n";
    rclcpp::shutdown();
    return 1;
  }
  file << yaml;
  if (!file) {
    std::cerr << "\nfailed while writing to " << path << "\n";
    rclcpp::shutdown();
    return 1;
  }
  std::cout << "\nwritten to " << path <<
    " -- reload it with load_demo to see spray_pattern round-trip "
    "correctly as the string \"no\".\n";

  rclcpp::shutdown();
  return 0;
}
