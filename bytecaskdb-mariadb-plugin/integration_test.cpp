// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// integration_test.cpp — Automated integration test for ByteCaskDB MariaDB plugin.
//
// This test provisions a local MariaDB instance, loads the ByteCaskDB plugin,
// runs comprehensive SQL tests, and validates results programmatically.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

class MariaDBInstance {
public:
  MariaDBInstance() {
    setup_paths();
    cleanup_previous();
    initialize_data_dir();
    start_server();
    wait_for_connection();
  }

  ~MariaDBInstance() {
    stop_server();
    cleanup_files();
  }

  // Execute SQL and return output lines
  std::vector<std::string> execute_sql(const std::string& sql) {
    std::string temp_file = test_dir_ + "/query_output.tmp";
    std::string cmd = "mariadb --socket=" + socket_path_ +
                     " --batch --skip-column-names -e \"" + sql +
                     "\" > " + temp_file + " 2>&1";

    int result = std::system(cmd.c_str());

    std::vector<std::string> lines;
    std::ifstream file(temp_file);
    std::string line;
    while (std::getline(file, line)) {
      lines.push_back(line);
    }

    // Clean up temp file
    fs::remove(temp_file);

    // Check if command succeeded
    if (result != 0 && !lines.empty() && lines[0].find("ERROR") != std::string::npos) {
      throw std::runtime_error("SQL failed: " + lines[0]);
    }

    return lines;
  }

  // Execute SQL and expect success (no return value needed)
  void execute_sql_expect_success(const std::string& sql) {
    auto result = execute_sql(sql);
    // If we get here without exception, it succeeded
  }

  // Execute SQL and expect specific single result
  std::string execute_sql_single_result(const std::string& sql) {
    auto lines = execute_sql(sql);
    if (lines.empty()) {
      throw std::runtime_error("Expected single result but got empty output");
    }
    return lines[0];
  }

private:
  std::string test_dir_;
  std::string data_dir_;
  std::string socket_path_;
  std::string pid_file_;
  std::string log_file_;
  std::string plugin_dir_;
  // server_pid_ used for tracking in destructor cleanup

  void setup_paths() {
    // Get the bytecask root directory
    char* bytecask_root = std::getenv("BYTECASK_ROOT");
    if (!bytecask_root) {
      // Try to find it relative to current location
      auto current = fs::current_path();
      while (current != current.root_path()) {
        if (fs::exists(current / "xmake.lua") && fs::exists(current / "bytecaskdb-mariadb-plugin")) {
          bytecask_root = const_cast<char*>(current.c_str());
          break;
        }
        current = current.parent_path();
      }
      if (!bytecask_root) {
        throw std::runtime_error("Could not find BYTECASK_ROOT");
      }
    }

    test_dir_ = std::string(bytecask_root) + "/.mariadb_integration_test";
    data_dir_ = test_dir_ + "/data";
    socket_path_ = test_dir_ + "/mysql.sock";
    pid_file_ = test_dir_ + "/mariadbd.pid";
    log_file_ = test_dir_ + "/error.log";
    plugin_dir_ = std::string(bytecask_root) + "/bytecaskdb-mariadb-plugin/build";
  }

  void cleanup_previous() {
    // Kill any existing test server
    if (fs::exists(pid_file_)) {
      std::ifstream pid_file(pid_file_);
      std::string pid_str;
      if (std::getline(pid_file, pid_str)) {
        pid_t old_pid = std::stoi(pid_str);
        kill(old_pid, SIGTERM);
        // Give it time to shutdown gracefully
        std::this_thread::sleep_for(std::chrono::seconds(2));
        kill(old_pid, SIGKILL);  // Force kill if still running
      }
    }

    // Remove test directory
    if (fs::exists(test_dir_)) {
      fs::remove_all(test_dir_);
    }
  }

  void cleanup_files() {
    if (fs::exists(test_dir_)) {
      fs::remove_all(test_dir_);
    }
  }

  void initialize_data_dir() {
    fs::create_directories(data_dir_);
    fs::create_directories(test_dir_ + "/tmp");

    // Initialize MariaDB data directory
    std::string cmd = "mariadb-install-db --datadir=" + data_dir_ +
                     " --auth-root-authentication-method=normal > /dev/null 2>&1";
    int result = std::system(cmd.c_str());
    if (result != 0) {
      throw std::runtime_error("Failed to initialize MariaDB data directory");
    }
  }

  void link_provider_plugins() {
    // MariaDB's default config (e.g. /etc/my.cnf.d/provider_*.cnf) may pin
    // compression-provider plugins as `force_plus_permanent`.  When we override
    // --plugin-dir to point at our build directory these must be reachable
    // there too, otherwise mariadbd aborts with "unknown variable
    // 'provider_<name>=force_plus_permanent'".
    const char *system_plugin_dirs[] = {
      "/usr/lib64/mariadb/plugin",
      "/usr/lib/mariadb/plugin",
    };
    for (const char *dir : system_plugin_dirs) {
      if (!fs::exists(dir)) continue;
      std::error_code ec;
      for (const auto &entry : fs::directory_iterator(dir, ec)) {
        const auto name = entry.path().filename().string();
        if (name.rfind("provider_", 0) == 0) {
          fs::path target = fs::path(plugin_dir_) / name;
          if (!fs::exists(target)) {
            fs::create_symlink(entry.path(), target, ec);
          }
        }
      }
      break;
    }
  }

  void start_server() {
    // Verify plugin exists
    if (!fs::exists(plugin_dir_ + "/ha_bytecaskdb.so")) {
      throw std::runtime_error("ByteCaskDB plugin not found at: " + plugin_dir_ + "/ha_bytecaskdb.so");
    }

    link_provider_plugins();

    // Start MariaDB server
    std::string cmd = "mariadbd --datadir=" + data_dir_ +
                     " --socket=" + socket_path_ +
                     " --port=3308" +  // Use different port from manual tests
                     " --pid-file=" + pid_file_ +
                     " --skip-grant-tables" +
                     " --tmpdir=" + test_dir_ + "/tmp" +
                     " --plugin-dir=" + plugin_dir_ +
                     " --plugin-load-add=bytecaskdb=ha_bytecaskdb.so" +
                     " --log-error=" + log_file_ +
                     " > /dev/null 2>&1 &";

    int result = std::system(cmd.c_str());
    if (result != 0) {
      throw std::runtime_error("Failed to start MariaDB server");
    }
  }

  void wait_for_connection() {
    // Wait up to 30 seconds for server to be ready
    for (int i = 0; i < 30; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));

      std::string test_cmd = "mariadb --socket=" + socket_path_ +
                            " -e 'SELECT 1;' > /dev/null 2>&1";
      if (std::system(test_cmd.c_str()) == 0) {
        return;  // Server is ready
      }
    }

    // If we get here, server failed to start - check log
    if (fs::exists(log_file_)) {
      std::ifstream log(log_file_);
      std::string line;
      std::cout << "MariaDB error log:\n";
      while (std::getline(log, line)) {
        std::cout << line << "\n";
      }
    }
    throw std::runtime_error("MariaDB server failed to start within 30 seconds");
  }

  void stop_server() {
    if (fs::exists(pid_file_)) {
      std::ifstream pid_file(pid_file_);
      std::string pid_str;
      if (std::getline(pid_file, pid_str)) {
        pid_t pid = std::stoi(pid_str);
        kill(pid, SIGTERM);

        // Wait for graceful shutdown
        for (int i = 0; i < 10; ++i) {
          if (kill(pid, 0) != 0) {
            break;  // Process has exited
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // Force kill if still running
        kill(pid, SIGKILL);
      }
    }
  }
};

TEST_CASE("MariaDB ByteCaskDB Plugin Integration Tests", "[integration]") {
  MariaDBInstance db;

  SECTION("Basic Plugin Functionality") {
    // Verify plugin is loaded
    auto result = db.execute_sql("SHOW ENGINES;");
    bool bytecaskdb_found = false;
    for (const auto& line : result) {
      if (line.find("bytecaskdb") != std::string::npos) {
        bytecaskdb_found = true;
        break;
      }
    }
    REQUIRE(bytecaskdb_found);

    // Create test database and table
    db.execute_sql_expect_success("DROP DATABASE IF EXISTS integration_test;");
    db.execute_sql_expect_success("CREATE DATABASE integration_test;");
    db.execute_sql_expect_success(
      "CREATE TABLE integration_test.basic_test (id INT PRIMARY KEY, value VARCHAR(50)) ENGINE=bytecaskdb;"
    );

    // Basic insert/select
    db.execute_sql_expect_success("INSERT INTO integration_test.basic_test VALUES (1, 'test');");
    auto value = db.execute_sql_single_result("SELECT value FROM integration_test.basic_test WHERE id = 1;");
    REQUIRE(value == "test");
  }

  SECTION("VARCHAR Ordering Fix Validation") {
    // Create table with VARCHAR index
    db.execute_sql_expect_success("DROP DATABASE IF EXISTS varchar_test;");
    db.execute_sql_expect_success("CREATE DATABASE varchar_test;");
    db.execute_sql_expect_success(
      "CREATE TABLE varchar_test.names (id INT PRIMARY KEY, name VARCHAR(50) NOT NULL) ENGINE=bytecaskdb;"
    );
    db.execute_sql_expect_success("CREATE INDEX idx_name ON varchar_test.names(name);");

    // Insert test data - specifically the problematic case
    db.execute_sql_expect_success(
      "INSERT INTO varchar_test.names VALUES (1, 'alice'), (2, 'bob'), (3, 'charlie');"
    );

    // Test ORDER BY LIMIT 1 - this was the failing case
    auto first_name = db.execute_sql_single_result(
      "SELECT name FROM varchar_test.names ORDER BY name LIMIT 1;"
    );
    REQUIRE(first_name == "alice");

    // Test full ORDER BY
    auto all_names = db.execute_sql("SELECT name FROM varchar_test.names ORDER BY name;");
    REQUIRE(all_names.size() == 3);
    REQUIRE(all_names[0] == "alice");
    REQUIRE(all_names[1] == "bob");
    REQUIRE(all_names[2] == "charlie");

    // Test ORDER BY DESC
    auto last_name = db.execute_sql_single_result(
      "SELECT name FROM varchar_test.names ORDER BY name DESC LIMIT 1;"
    );
    REQUIRE(last_name == "charlie");
  }

  SECTION("Comprehensive VARCHAR Lexicographic Ordering") {
    db.execute_sql_expect_success("DROP DATABASE IF EXISTS lex_test;");
    db.execute_sql_expect_success("CREATE DATABASE lex_test;");
    db.execute_sql_expect_success(
      "CREATE TABLE lex_test.words (id INT PRIMARY KEY, word VARCHAR(20) NOT NULL) ENGINE=bytecaskdb;"
    );
    db.execute_sql_expect_success("CREATE INDEX idx_word ON lex_test.words(word);");

    // Insert words of different lengths that should expose length prefix issues
    db.execute_sql_expect_success(
      "INSERT INTO lex_test.words VALUES "
      "(1, 'a'), (2, 'aa'), (3, 'aaa'), (4, 'b'), (5, 'bb'), (6, 'z'), (7, 'zzz');"
    );

    // Test that single 'a' comes before 'aa', 'aaa', 'b', etc.
    auto first = db.execute_sql_single_result("SELECT word FROM lex_test.words ORDER BY word LIMIT 1;");
    REQUIRE(first == "a");

    // Test that 'aa' comes before 'aaa'
    auto results = db.execute_sql("SELECT word FROM lex_test.words ORDER BY word LIMIT 3;");
    REQUIRE(results.size() == 3);
    REQUIRE(results[0] == "a");
    REQUIRE(results[1] == "aa");
    REQUIRE(results[2] == "aaa");

    // Test range query
    auto b_words = db.execute_sql("SELECT word FROM lex_test.words WHERE word >= 'b' ORDER BY word;");
    REQUIRE(b_words.size() >= 3);
    REQUIRE(b_words[0] == "b");
    REQUIRE(b_words[1] == "bb");
    // 'z' should come after 'bb'
    bool found_z = false;
    for (const auto& word : b_words) {
      if (word == "z" || word == "zzz") {
        found_z = true;
        break;
      }
    }
    REQUIRE(found_z);
  }

  SECTION("Multi-Column Index Operations") {
    db.execute_sql_expect_success("DROP DATABASE IF EXISTS multi_test;");
    db.execute_sql_expect_success("CREATE DATABASE multi_test;");
    db.execute_sql_expect_success(
      "CREATE TABLE multi_test.users ("
      "id INT PRIMARY KEY, "
      "name VARCHAR(50) NOT NULL, "
      "status VARCHAR(20) NOT NULL, "
      "score INT"
      ") ENGINE=bytecaskdb;"
    );

    // Create multiple indexes
    db.execute_sql_expect_success("CREATE INDEX idx_name ON multi_test.users(name);");
    db.execute_sql_expect_success("CREATE INDEX idx_status ON multi_test.users(status);");

    // Insert test data
    db.execute_sql_expect_success(
      "INSERT INTO multi_test.users VALUES "
      "(1, 'alice', 'active', 100), "
      "(2, 'bob', 'inactive', 50), "
      "(3, 'charlie', 'active', 75);"
    );

    // Test index scan by name
    auto alice = db.execute_sql_single_result(
      "SELECT status FROM multi_test.users WHERE name = 'alice';"
    );
    REQUIRE(alice == "active");

    // Test index scan by status
    auto active_users = db.execute_sql("SELECT name FROM multi_test.users WHERE status = 'active' ORDER BY name;");
    REQUIRE(active_users.size() == 2);
    REQUIRE(active_users[0] == "alice");
    REQUIRE(active_users[1] == "charlie");
  }

  SECTION("CRUD Operations with Index Maintenance") {
    db.execute_sql_expect_success("DROP DATABASE IF EXISTS crud_test;");
    db.execute_sql_expect_success("CREATE DATABASE crud_test;");
    db.execute_sql_expect_success(
      "CREATE TABLE crud_test.items (id INT PRIMARY KEY, name VARCHAR(30) NOT NULL, category VARCHAR(20) NOT NULL) ENGINE=bytecaskdb;"
    );
    db.execute_sql_expect_success("CREATE INDEX idx_name ON crud_test.items(name);");
    db.execute_sql_expect_success("CREATE INDEX idx_category ON crud_test.items(category);");

    // Insert
    db.execute_sql_expect_success("INSERT INTO crud_test.items VALUES (1, 'apple', 'fruit');");
    db.execute_sql_expect_success("INSERT INTO crud_test.items VALUES (2, 'banana', 'fruit');");
    db.execute_sql_expect_success("INSERT INTO crud_test.items VALUES (3, 'carrot', 'vegetable');");

    // Read - verify indexes work
    auto fruits = db.execute_sql("SELECT name FROM crud_test.items WHERE category = 'fruit' ORDER BY name;");
    REQUIRE(fruits.size() == 2);
    REQUIRE(fruits[0] == "apple");
    REQUIRE(fruits[1] == "banana");

    // Update - this should maintain indexes correctly
    db.execute_sql_expect_success("UPDATE crud_test.items SET category = 'citrus' WHERE name = 'banana';");

    // Verify update
    auto new_category = db.execute_sql_single_result("SELECT category FROM crud_test.items WHERE name = 'banana';");
    REQUIRE(new_category == "citrus");

    // Verify old index entry removed and new one added
    auto fruits_after_update = db.execute_sql("SELECT name FROM crud_test.items WHERE category = 'fruit';");
    REQUIRE(fruits_after_update.size() == 1);
    REQUIRE(fruits_after_update[0] == "apple");

    auto citrus = db.execute_sql("SELECT name FROM crud_test.items WHERE category = 'citrus';");
    REQUIRE(citrus.size() == 1);
    REQUIRE(citrus[0] == "banana");

    // Delete
    db.execute_sql_expect_success("DELETE FROM crud_test.items WHERE name = 'apple';");

    // Verify delete
    auto remaining_items = db.execute_sql("SELECT name FROM crud_test.items ORDER BY name;");
    REQUIRE(remaining_items.size() == 2);
    REQUIRE(remaining_items[0] == "banana");
    REQUIRE(remaining_items[1] == "carrot");
  }

  SECTION("Edge Cases and Error Handling") {
    db.execute_sql_expect_success("DROP DATABASE IF EXISTS edge_test;");
    db.execute_sql_expect_success("CREATE DATABASE edge_test;");

    // Test NULL handling (should fail for indexed columns)
    db.execute_sql_expect_success(
      "CREATE TABLE edge_test.nullable (id INT PRIMARY KEY, value VARCHAR(50)) ENGINE=bytecaskdb;"
    );

    // Nullable columns can be indexed
    db.execute_sql_expect_success("CREATE INDEX idx_value ON edge_test.nullable(value);");
    db.execute_sql_expect_success("INSERT INTO edge_test.nullable VALUES (1, NULL), (2, 'hello');");

    auto nullable_result = db.execute_sql_single_result(
      "SELECT value FROM edge_test.nullable WHERE id = 2;"
    );
    REQUIRE(nullable_result == "hello");

    // Test empty strings
    db.execute_sql_expect_success(
      "CREATE TABLE edge_test.empty_strings (id INT PRIMARY KEY, name VARCHAR(50) NOT NULL) ENGINE=bytecaskdb;"
    );
    db.execute_sql_expect_success("CREATE INDEX idx_name ON edge_test.empty_strings(name);");
    db.execute_sql_expect_success("INSERT INTO edge_test.empty_strings VALUES (1, ''), (2, 'a');");

    auto first_empty = db.execute_sql_single_result(
      "SELECT name FROM edge_test.empty_strings ORDER BY name LIMIT 1;"
    );
    // Empty string should sort before 'a'
    REQUIRE(first_empty == "");
  }

  // Cleanup all test databases
  db.execute_sql_expect_success("DROP DATABASE IF EXISTS integration_test;");
  db.execute_sql_expect_success("DROP DATABASE IF EXISTS varchar_test;");
  db.execute_sql_expect_success("DROP DATABASE IF EXISTS lex_test;");
  db.execute_sql_expect_success("DROP DATABASE IF EXISTS multi_test;");
  db.execute_sql_expect_success("DROP DATABASE IF EXISTS crud_test;");
  db.execute_sql_expect_success("DROP DATABASE IF EXISTS edge_test;");
}
}