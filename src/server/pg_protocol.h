#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace litedb::server {

// PostgreSQL wire protocol v3 message types (frontend → backend).
namespace pg {
constexpr char kQuery = 'Q';
constexpr char kTerminate = 'X';
}  // namespace pg

// Message builders for backend → frontend responses.
class PgWriter {
 public:
  // AuthenticationOk (R)
  static std::string auth_ok();

  // ParameterStatus (S)
  static std::string parameter_status(const std::string& key,
                                      const std::string& value);

  // ReadyForQuery (Z) — 'I' = idle
  static std::string ready_for_query(char status = 'I');

  // RowDescription (T)
  static std::string row_description(
      const std::vector<std::string>& column_names);

  // DataRow (D)
  static std::string data_row(const std::vector<std::string>& values);

  // CommandComplete (C)
  static std::string command_complete(const std::string& tag);

  // ErrorResponse (E)
  static std::string error_response(const std::string& severity,
                                    const std::string& code,
                                    const std::string& message);

 private:
  static void write_int32(std::string& buf, int32_t val);
  static void write_int16(std::string& buf, int16_t val);
  static void write_cstring(std::string& buf, const std::string& str);
};

}  // namespace litedb::server
