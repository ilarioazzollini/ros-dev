#include <iostream>
#include <string>

#include "rclcpp/parameter_map.hpp"

int main(int argc, char ** argv)
{
  if (argc < 2) {
    std::cerr << "usage: load_demo <path/to/params.yaml>\n";
    return 1;
  }

  const std::string path = argv[1];
  const auto map = rclcpp::parameter_map_from_yaml_file(path);

  std::cout << "loaded " << map.size() << " node(s) from " << path << ":\n";
  for (const auto & [node_fqn, params] : map) {
    std::cout << node_fqn << ":\n";
    for (const auto & p : params) {
      std::cout << "  " << p.get_name() << " = " << p.value_to_string() <<
        "  (" << p.get_type_name() << ")\n";
    }
  }
  return 0;
}
