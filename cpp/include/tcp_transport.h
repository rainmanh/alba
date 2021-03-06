/*
Copyright (C) 2016 iNuron NV

This file is part of Open vStorage Open Source Edition (OSE), as available from


    http://www.openvstorage.org and
    http://www.openvstorage.com.

This file is free software; you can redistribute it and/or modify it
under the terms of the GNU Affero General Public License v3 (GNU AGPLv3)
as published by the Free Software Foundation, in version 3 as it comes
in the <LICENSE.txt> file of the Open vStorage OSE distribution.

Open vStorage is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY of any kind.
*/

#pragma once
#include "transport.h"

namespace alba {
namespace transport {
class TCP_transport : public Transport {

public:
  TCP_transport(const std::string &ip, const std::string &port,
                const std::chrono::steady_clock::duration &timeout);

  void write_exact(const char *buf, int len) override;
  void read_exact(char *buf, int len) override;

  void
  expires_from_now(const std::chrono::steady_clock::duration &timeout) override;

  ~TCP_transport();

private:
  boost::asio::io_service _io_service;
  boost::asio::ip::tcp::socket _socket;
  boost::asio::deadline_timer _deadline;
  void output(llio::message_builder &mb);
  llio::message input();
  boost::posix_time::milliseconds _timeout;
  void _check_deadline();
};
}
}
