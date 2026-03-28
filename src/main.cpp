import std;

auto main() -> int {
  auto words = std::vector<std::string>{"gustavo", "amigo"};
  for (const auto &w : words) {
    std::println("word = {} a size = {}", w, w.length());
  }
}
