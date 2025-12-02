#pragma once

#include <cstdint>
#include <string>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace kzevent::net {

class inet_address {
public:
  inet_address(const std::string &ip, uint16_t port);

private:
};

} // namespace kzevent::net