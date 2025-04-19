#include "server/pg_protocol.h"

#include <arpa/inet.h>
#include <cstring>

namespace litedb::server {

void PgWriter::write_int32(std::string& buf, int32_t val) {
  uint32_t net = htonl(static_cast<uint32_t>(val));
  buf.append(reinterpret_cast<const char*>(&net), 4);
}

void PgWriter::write_int16(std::string& buf, int16_t val) {
  uint16_t net = htons(static_cast<uint16_t>(val));
  buf.append(reinterpret_cast<const char*>(&net), 2);
}

void PgWriter::write_cstring(std::string& buf, const std::string& str) {
  buf.append(str);
  buf.push_back('\0');
}

std::string PgWriter::auth_ok() {
  std::string msg;
  msg += 'R';
  write_int32(msg, 8);   // length includes self
  write_int32(msg, 0);   // AuthenticationOk
  return msg;
}

std::string PgWriter::parameter_status(const std::string& key,
                                       const std::string& value) {
  std::string body;
  write_cstring(body, key);
  write_cstring(body, value);
  std::string msg;
  msg += 'S';
  write_int32(msg, static_cast<int32_t>(4 + body.size()));
  msg += body;
  return msg;
}

std::string PgWriter::ready_for_query(char status) {
  std::string msg;
  msg += 'Z';
  write_int32(msg, 5);
  msg += status;
  return msg;
}

std::string PgWriter::row_description(
    const std::vector<std::string>& column_names) {
  std::string body;
  write_int16(body, static_cast<int16_t>(column_names.size()));
  for (const auto& col : column_names) {
    write_cstring(body, col);
    write_int32(body, 0);   // table OID
    write_int16(body, 0);   // column attr number
    write_int32(body, 25);  // type OID (25 = text)
    write_int16(body, -1);  // type size
    write_int32(body, -1);  // type modifier
    write_int16(body, 0);   // format code (text)
  }
  std::string msg;
  msg += 'T';
  write_int32(msg, static_cast<int32_t>(4 + body.size()));
  msg += body;
  return msg;
}

std::string PgWriter::data_row(const std::vector<std::string>& values) {
  std::string body;
  write_int16(body, static_cast<int16_t>(values.size()));
  for (const auto& v : values) {
    write_int32(body, static_cast<int32_t>(v.size()));
    body += v;
  }
  std::string msg;
  msg += 'D';
  write_int32(msg, static_cast<int32_t>(4 + body.size()));
  msg += body;
  return msg;
}

std::string PgWriter::command_complete(const std::string& tag) {
  std::string body;
  write_cstring(body, tag);
  std::string msg;
  msg += 'C';
  write_int32(msg, static_cast<int32_t>(4 + body.size()));
  msg += body;
  return msg;
}

std::string PgWriter::error_response(const std::string& severity,
                                     const std::string& code,
                                     const std::string& message) {
  std::string body;
  body += 'S';
  write_cstring(body, severity);
  body += 'C';
  write_cstring(body, code);
  body += 'M';
  write_cstring(body, message);
  body += '\0';  // terminator
  std::string msg;
  msg += 'E';
  write_int32(msg, static_cast<int32_t>(4 + body.size()));
  msg += body;
  return msg;
}

}  // namespace litedb::server
